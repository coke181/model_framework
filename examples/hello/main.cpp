#include "hello_routes.hpp"

#include "mf/agent/model_provider.hpp"
#include "mf/agent/service.hpp"
#include "mf/agent/tool_registry.hpp"
#include "mf/auth/account_service.hpp"
#include "mf/auth/password_hasher.hpp"
#include "mf/auth/token_service.hpp"
#include "mf/core/application.hpp"
#include "mf/core/env.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/gacha_service.hpp"
#include "mf/game/loot_service.hpp"
#include "mf/game/mail_service.hpp"
#include "mf/game/shop_service.hpp"
#include "mf/multiplayer/service.hpp"
#include "mf/storage/factory.hpp"
#include "mf/storage/options.hpp"
#include "mf/storage/redis_repositories.hpp"
#include "mf/voice/service.hpp"

#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <iostream>
#include <mutex>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace {

// 启动时确保本地开发管理员存在。
// 调用流程：
// 1. 先读取环境变量或默认的开发管理员账号配置。
// 2. 如果账号不存在，则走正式 register_account 创建密码账号。
// 3. 如果账号已存在但密码不匹配，则把密码重置成当前开发默认值。
// 4. 最后无论账号是否新建，都补齐管理员权限映射，保证 /admin/login 能直接成功。
void ensure_dev_admin_account(
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const std::shared_ptr<mf::auth::PasswordHasher>& password_hasher,
    const std::shared_ptr<mf::core::Logger>& logger,
    mf::core::RuntimeEnvironment runtime_environment) {
    const auto enabled = [runtime_environment] {
        // production 下强制关闭开发管理员自动创建。
        // 这样即使 deploy.env 误写 HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN=1，
        // 正式服也不会把默认账号 admin_demo / AdminDemo_123456 暴露出来。
        if (mf::core::is_production(runtime_environment)) {
            return false;
        }
        if (const char* value = std::getenv("HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN")) {
            return std::string_view{value} != "0" && std::string_view{value} != "false";
        }
        // 未显式配置时，只有 development 默认开启；staging 默认关闭。
        return mf::core::is_development(runtime_environment);
    }();
    if (!enabled) {
        logger->log(
            mf::core::LogLevel::info,
            "dev admin bootstrap disabled runtime_env=" + mf::core::to_string(runtime_environment));
        return;
    }

    const auto login_name = [] {
        if (const char* value = std::getenv("HELLO_SERVICE_DEV_ADMIN_LOGIN")) {
            return std::string{value};
        }
        return std::string{"admin_demo"};
    }();
    const auto password = [] {
        if (const char* value = std::getenv("HELLO_SERVICE_DEV_ADMIN_PASSWORD")) {
            return std::string{value};
        }
        return std::string{"AdminDemo_123456"};
    }();
    const auto display_name = [] {
        if (const char* value = std::getenv("HELLO_SERVICE_DEV_ADMIN_DISPLAY_NAME")) {
            return std::string{value};
        }
        return std::string{"开发管理员"};
    }();

    std::optional<mf::game::Account> account = repositories.accounts->find_by_login_name(login_name);
    if (!account) {
        const auto created = account_service->register_account({
            .login_name = login_name,
            .display_name = display_name,
            .email = {},
            .phone_number = {},
            .platform_channel = "dev",
            .register_source = "startup_dev_admin",
            .password = password,
            .client_ip = "127.0.0.1",
            .user_agent = "startup-bootstrap",
        });
        if (created.error || !created.value) {
            logger->log(
                mf::core::LogLevel::error,
                "failed to bootstrap dev admin login_name=" + login_name +
                    " error=" + (created.error ? created.error->code : std::string{"unknown"}));
            return;
        }
        account = created.value->account;
        logger->log(
            mf::core::LogLevel::info,
            "bootstrapped dev admin password account login_name=" + login_name);
    } else {
        const auto auth_record = repositories.accounts->find_password_account_by_login(login_name);
        const auto password_ok =
            auth_record && mf::auth::PasswordHasher::is_supported_scheme(auth_record->password_scheme) &&
            password_hasher->verify_password(password, auth_record->password_hash);
        if (!password_ok) {
            const auto updated = repositories.accounts->update_password_credentials(
                account->account_id,
                password_hasher->hash_password(password),
                password_hasher->scheme_name());
            if (updated) {
                account = updated;
                logger->log(
                    mf::core::LogLevel::info,
                    "reset dev admin password to configured startup default login_name=" + login_name);
            } else {
                logger->log(
                    mf::core::LogLevel::error,
                    "failed to reset dev admin password login_name=" + login_name);
                return;
            }
        }
    }

    static const std::vector<std::string> kDevAdminPermissions{
        "admin.query",
        "admin.audit_read",
        "admin.user_manage",
        "admin.account_status",
        "admin.blacklist",
        "admin.chat_mute",
        "admin.chat_announce",
        "admin.chat_outbox_retry",
        "admin.task_announce",
        "admin.mail_send",
        "admin.batch_mail",
        "admin.reward_grant",
        "admin.batch_reward",
        "admin.reward_template",
        "admin.item_config",
        "admin.gacha_config",
        "admin.shop_config",
        "admin.loot_spawn",
        "admin.quest_config",
        "admin.voice_manage",
    };
    repositories.admins->upsert_admin(account->account_id, "dev_admin", true, kDevAdminPermissions);
    logger->log(
        mf::core::LogLevel::info,
        "dev admin is ready login_name=" + login_name + " password=" + password);
}

std::string normalized_target(const mf::http::Request& request) {
    const std::string target = std::string{request.target()};
    const auto query_pos = target.find('?');
    return query_pos == std::string::npos ? target : target.substr(0, query_pos);
}

std::optional<std::string> read_env_string(const char* name) {
    if (const char* value = std::getenv(name)) {
        return std::string{value};
    }
    return std::nullopt;
}

std::uint64_t read_env_u64_or(const char* name, std::uint64_t fallback) {
    if (const auto value = read_env_string(name)) {
        try {
            const auto parsed = static_cast<std::uint64_t>(std::stoull(*value));
            return parsed == 0 ? fallback : parsed;
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::uint64_t read_env_u64_allow_zero(const char* name, std::uint64_t fallback) {
    if (const auto value = read_env_string(name)) {
        try {
            return static_cast<std::uint64_t>(std::stoull(*value));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

}  // namespace

int main() {
    try {
    // 调用流程：
    // 1. 读取存储和服务配置，构造 repositories / services。
    // 2. 构造 HTTP API 所需服务；聊天网关与任务 worker 已拆成独立进程。
    // 3. 把所有路由注册函数按职责挂到 Application。
    // 4. 最后进入 app.run()，由路由模块接管具体业务处理。
    const auto storage_options = mf::storage::load_storage_options_from_env(
        "HELLO_SERVICE",
        mf::storage::StorageOptions{
            .mode = mf::storage::StorageMode::in_memory,
        });
    const auto bootstrap_logger = mf::core::make_console_logger();
    const auto service_name = std::string{"ue-game-backend"};
    // 本机极限吞吐压测时关闭请求级观测，避免把日志、trace 和指标锁竞争
    // 混入业务结果；生产环境不要开启这个开关。
    const bool perf_mode = [] {
        if (const char* value = std::getenv("HELLO_SERVICE_PERF_MODE")) {
            return std::string_view{value} == "1" || std::string_view{value} == "true";
        }
        return false;
    }();
    const auto runtime_environment = mf::core::load_runtime_environment_from_env(
        "HELLO_SERVICE",
        mf::core::RuntimeEnvironment::development);
    if (perf_mode && mf::core::is_production(runtime_environment)) {
        throw std::runtime_error("HELLO_SERVICE_PERF_MODE is forbidden in production");
    }
    bootstrap_logger->log(
        mf::core::LogLevel::info,
        "runtime environment=" + mf::core::to_string(runtime_environment));
    const auto metrics = mf::core::make_metrics_registry(service_name);
    const auto tracer = mf::core::make_tracer_provider(
        perf_mode ? std::shared_ptr<mf::core::Logger>{} : bootstrap_logger,
        service_name);
    const auto load_probe = std::make_shared<mf::http::LoadProbe>();
    mf::core::configure_global_tracer(tracer);
    const auto repositories = mf::storage::make_repositories(storage_options, bootstrap_logger, metrics);
    const auto token_service = std::make_shared<mf::auth::TokenService>(repositories.sessions);

    // mysql_redis 模式下订阅会话撤销广播，让水平扩展的多个实例都能及时清掉本地 token 缓存。
    std::shared_ptr<mf::storage::SessionRevocationSubscriber> session_revocation_subscriber;
    if (storage_options.mode == mf::storage::StorageMode::mysql_redis &&
        !storage_options.in_memory_sessions) {
        session_revocation_subscriber = mf::storage::make_redis_session_revocation_subscriber(
            storage_options.redis,
            [token_service](std::string player_id, std::optional<std::string> except_session_id) {
                token_service->invalidate_player_sessions(player_id, except_session_id);
            });
        session_revocation_subscriber->start();
    }

    mf::auth::PasswordHashOptions password_options;
    password_options.memory_cost_kib = static_cast<std::uint32_t>(
        read_env_u64_or("HELLO_SERVICE_PASSWORD_ARGON2_MEMORY_KIB", password_options.memory_cost_kib));
    password_options.time_cost = static_cast<std::uint32_t>(
        read_env_u64_or("HELLO_SERVICE_PASSWORD_ARGON2_TIME_COST", password_options.time_cost));
    password_options.parallelism = static_cast<std::uint32_t>(
        read_env_u64_or("HELLO_SERVICE_PASSWORD_ARGON2_PARALLELISM", password_options.parallelism));
    password_options.verification_cache_ttl_ms = static_cast<std::uint32_t>(
        read_env_u64_allow_zero("HELLO_SERVICE_PASSWORD_VERIFY_CACHE_TTL_MS", password_options.verification_cache_ttl_ms));
    password_options.verification_cache_max_entries = static_cast<std::uint32_t>(
        read_env_u64_allow_zero("HELLO_SERVICE_PASSWORD_VERIFY_CACHE_MAX_ENTRIES", password_options.verification_cache_max_entries));
    if (mf::core::is_production(runtime_environment) &&
        (password_options.memory_cost_kib < 19456 ||
         password_options.time_cost < 2 ||
         password_options.parallelism < 1)) {
        throw std::runtime_error(
            "production requires Argon2id memory>=19456 KiB, time_cost>=2 and parallelism>=1");
    }
    const bool insecure_fast_password_hash = [] {
        if (const auto value = read_env_string("HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH")) {
            return *value == "1" || *value == "true" || *value == "on";
        }
        return false;
    }();
    if (insecure_fast_password_hash && (!perf_mode || mf::core::is_production(runtime_environment))) {
        throw std::runtime_error(
            "HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH requires PERF_MODE and is forbidden in production");
    }
    password_options.benchmark_fast_hash = insecure_fast_password_hash;
    if (insecure_fast_password_hash) {
        bootstrap_logger->log(
            mf::core::LogLevel::warn,
            "INSECURE password benchmark mode enabled: Argon2id is disabled for newly written passwords");
    }
    const auto password_hasher = std::make_shared<mf::auth::PasswordHasher>(password_options);
    // 密码哈希线程数独立于 HTTP/业务线程数，默认保留约 20% 逻辑 CPU 给网络和存储。
    // 也可以通过 HELLO_SERVICE_PASSWORD_HASH_THREADS 显式覆盖。
    const auto default_password_hash_threads = [] {
        const auto cpu_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (cpu_count <= 4) {
            return std::max<std::size_t>(1, cpu_count / 2);
        }
        return std::max<std::size_t>(1, cpu_count - std::max<std::size_t>(2, cpu_count / 5));
    }();
    const auto password_hash_threads = static_cast<std::size_t>(read_env_u64_or(
        "HELLO_SERVICE_PASSWORD_HASH_THREADS",
        default_password_hash_threads));
    const auto password_hash_max_pending = static_cast<std::size_t>(read_env_u64_or(
        "HELLO_SERVICE_PASSWORD_HASH_MAX_PENDING",
        std::max<std::uint64_t>(1, password_hash_threads * 2)));
    const auto register_rate_window = std::chrono::seconds{static_cast<std::int64_t>(read_env_u64_or(
        "HELLO_SERVICE_REGISTER_RATE_WINDOW_SECONDS", 60))};
    const auto register_max_per_login_ip = static_cast<std::int64_t>(read_env_u64_or(
        "HELLO_SERVICE_REGISTER_MAX_PER_LOGIN_IP", 3));
    const auto register_max_per_ip = static_cast<std::int64_t>(read_env_u64_or(
        "HELLO_SERVICE_REGISTER_MAX_PER_IP", 30));
    const auto account_service = std::make_shared<mf::auth::AccountService>(
        repositories.accounts,
        repositories.players,
        repositories.auth_security,
        password_hasher,
        token_service,
        password_hash_threads,
        password_hash_max_pending,
        register_rate_window,
        register_max_per_login_ip,
        register_max_per_ip,
        perf_mode ? nullptr : metrics);
    const std::shared_ptr<mf::chat::ChatStateService> chat_state_service =
        storage_options.mode == mf::storage::StorageMode::mysql_redis
            ? mf::storage::make_redis_chat_state_service(storage_options.redis)
            : mf::chat::make_in_memory_chat_state_service(
                  static_cast<std::size_t>(read_env_u64_or("HELLO_CHAT_STATE_SHARD_COUNT", 64)));
    const auto chat_service = std::make_shared<mf::chat::ChatService>(
        repositories.chat,
        chat_state_service,
        repositories.friends,
        repositories.chat_moderation,
        4096,
        64,
        perf_mode ? nullptr : metrics,
        bootstrap_logger);
    const auto reward_service = std::make_shared<mf::game::RewardService>(
        repositories.players,
        repositories.rewards,
        repositories.reward_templates,
        repositories.item_catalog,
        repositories.reward_transactions);
    const auto mail_service = std::make_shared<mf::game::MailService>(
        repositories.mails,
        repositories.players,
        repositories.rewards,
        reward_service);
    const auto gacha_service = std::make_shared<mf::game::GachaService>(
        repositories.gacha,
        repositories.players,
        reward_service,
        repositories.gacha_transactions);
    const auto shop_service = std::make_shared<mf::game::ShopService>(
        repositories.shop,
        repositories.players,
        reward_service,
        repositories.shop_transactions);
    const auto loot_service = std::make_shared<mf::game::LootService>(
        repositories.loot,
        repositories.players,
        reward_service,
        repositories.loot_transactions);
    const auto quest_service = std::make_shared<mf::game::QuestService>(repositories.quests);
    const auto agent_options = mf::agent::load_agent_options_from_env();
    const auto agent_tool_registry = std::make_shared<mf::agent::AgentToolRegistry>(repositories, quest_service);
    const auto agent_service = std::make_shared<mf::agent::AgentService>(
        agent_options,
        repositories.agent,
        mf::agent::make_agent_model_provider(agent_options),
        agent_tool_registry);
    const auto agent_task_service = std::make_shared<mf::agent::AgentTaskService>();
    const auto voice_token_provider = std::make_shared<mf::voice::LiveKitTokenProvider>(
        mf::voice::load_voice_options_from_env());
    const auto voice_service = std::make_shared<mf::voice::VoiceService>(
        repositories.voice,
        voice_token_provider,
        repositories.multiplayer_rooms);
    const auto multiplayer_service = std::make_shared<mf::multiplayer::MultiplayerRoomService>(
        repositories.multiplayer_rooms,
        voice_service);
    const auto admin_task_scheduler = make_admin_task_scheduler(
        chat_service,
        account_service,
        repositories,
        repositories.audit_logs,
        repositories.admin_tasks,
        metrics,
        bootstrap_logger);
    const auto bootstrap_token = [] {
        if (const char* value = std::getenv("HELLO_SERVICE_GM_TOKEN")) {
            return std::string{value};
        }
        return std::string{};
    }();
    const auto request_log_sample_rate = perf_mode
        ? std::numeric_limits<std::uint64_t>::max()
        : read_env_u64_or("HELLO_SERVICE_REQUEST_LOG_SAMPLE_RATE", 1);

    // 本地 F5 联调默认自动补一个开发管理员，避免 in_memory 模式每次重启都要手工注册和 bootstrap。
    ensure_dev_admin_account(repositories, account_service, password_hasher, bootstrap_logger, runtime_environment);

    mf::core::Application app{
        mf::core::load_app_options_from_env(
            "HELLO_SERVICE",
            mf::core::AppOptions{
                .name = "ue-game-backend",
                .server =
                    mf::http::ServerOptions{
                        .host = boost::asio::ip::make_address("127.0.0.1"),
                        .port = 8080,
                        .metrics = metrics,
                        .load_probe = load_probe,
                    },
            }),
    };

    app.use([logger = app.logger(), metrics, tracer, request_log_sample_rate, perf_mode, request_counter = std::make_shared<std::atomic<std::uint64_t>>(0)](const mf::http::Request& request, const mf::http::Handler& next) {
        const auto started_at = std::chrono::steady_clock::now();
        const auto incoming_traceparent = std::string{request["traceparent"]};
        const auto incoming_context = mf::core::TracerProvider::parse_traceparent(incoming_traceparent);
        auto span = perf_mode ? mf::core::SpanScope{} : tracer->start_span(
            std::string{mf::http::method_name(request.method())} + " " + normalized_target(request),
            {
                {"http.method", std::string{mf::http::method_name(request.method())}},
                {"http.target", std::string{request.target()}},
            },
            incoming_context);
        const auto trace_id = span.context().trace_id;
        auto response = next(request);
        const auto duration_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count();
        if (!perf_mode) {
            span.set_attribute("http.status_code", std::to_string(response.result_int()));
            span.set_attribute("http.response_time_ms", std::to_string(duration_ms));
            response.set("traceparent", mf::core::TracerProvider::format_traceparent(span.context()));
            response.set("X-Trace-Id", trace_id);
            response.set("X-Response-Time-Ms", std::to_string(duration_ms));
        }
        const auto request_index = request_counter->fetch_add(1, std::memory_order_relaxed) + 1;
        const auto should_log_regular_request = request_log_sample_rate <= 1 || (request_index % request_log_sample_rate) == 0;
        if (!perf_mode) metrics->increment_counter(
            "mf_http_requests_total",
            1.0,
            {
                {"method", std::string{mf::http::method_name(request.method())}},
                {"path", normalized_target(request)},
                {"status", std::to_string(response.result_int())},
            });
        if (!perf_mode) metrics->observe_histogram_ms(
            "mf_http_request_duration_ms",
            duration_ms,
            {
                {"method", std::string{mf::http::method_name(request.method())}},
                {"path", normalized_target(request)},
            });
        if (duration_ms >= 500) {
            metrics->increment_counter(
                "mf_http_slow_requests_total",
                1.0,
                {
                    {"method", std::string{mf::http::method_name(request.method())}},
                    {"path", normalized_target(request)},
                    {"status", std::to_string(response.result_int())},
                });
            metrics->increment_counter(
                "mf_alert_events_total",
                1.0,
                {
                    {"category", "slow_http_request"},
                    {"path", normalized_target(request)},
                });
            logger->log(
                mf::core::LogLevel::warn,
                "slow_request trace_id=" + trace_id + " method=" +
                    std::string{mf::http::method_name(request.method())} + " path=" + normalized_target(request) +
                    " status=" + std::to_string(response.result_int()) + " duration_ms=" + std::to_string(duration_ms));
        } else if (should_log_regular_request) {
            logger->log(
                mf::core::LogLevel::info,
                "request_done trace_id=" + trace_id + " method=" +
                    std::string{mf::http::method_name(request.method())} + " path=" + normalized_target(request) +
                    " status=" + std::to_string(response.result_int()) + " duration_ms=" + std::to_string(duration_ms));
        }
        return response;
    });

    // 异步路由使用独立的可 co_await 中间件链。
    // 这里保持与同步中间件相同的 tracing、metrics 和日志语义，避免 refresh 因改成协程而绕过观测。
    app.use_async([logger = app.logger(), metrics, tracer, request_log_sample_rate, perf_mode,
                   request_counter = std::make_shared<std::atomic<std::uint64_t>>(0)](
                      const mf::http::Request& request,
                      const mf::http::AsyncHandler& next) -> boost::asio::awaitable<mf::http::Response> {
        const auto started_at = std::chrono::steady_clock::now();
        const auto incoming_traceparent = std::string{request["traceparent"]};
        const auto incoming_context = mf::core::TracerProvider::parse_traceparent(incoming_traceparent);
        auto span = perf_mode ? mf::core::SpanScope{} : tracer->start_span(
            std::string{mf::http::method_name(request.method())} + " " + normalized_target(request),
            {
                {"http.method", std::string{mf::http::method_name(request.method())}},
                {"http.target", std::string{request.target()}},
            },
            incoming_context);
        const auto trace_id = span.context().trace_id;
        auto response = co_await next(request);
        const auto duration_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count();
        if (!perf_mode) {
            span.set_attribute("http.status_code", std::to_string(response.result_int()));
            span.set_attribute("http.response_time_ms", std::to_string(duration_ms));
            response.set("traceparent", mf::core::TracerProvider::format_traceparent(span.context()));
            response.set("X-Trace-Id", trace_id);
            response.set("X-Response-Time-Ms", std::to_string(duration_ms));
            metrics->increment_counter(
                "mf_http_requests_total",
                1.0,
                {
                    {"method", std::string{mf::http::method_name(request.method())}},
                    {"path", normalized_target(request)},
                    {"status", std::to_string(response.result_int())},
                });
            metrics->observe_histogram_ms(
                "mf_http_request_duration_ms",
                duration_ms,
                {
                    {"method", std::string{mf::http::method_name(request.method())}},
                    {"path", normalized_target(request)},
                });
        }
        const auto request_index = request_counter->fetch_add(1, std::memory_order_relaxed) + 1;
        const auto should_log_regular_request =
            request_log_sample_rate <= 1 || (request_index % request_log_sample_rate) == 0;
        if (duration_ms >= 500) {
            metrics->increment_counter(
                "mf_http_slow_requests_total",
                1.0,
                {
                    {"method", std::string{mf::http::method_name(request.method())}},
                    {"path", normalized_target(request)},
                    {"status", std::to_string(response.result_int())},
                });
            metrics->increment_counter(
                "mf_alert_events_total", 1.0,
                {{"category", "slow_http_request"}, {"path", normalized_target(request)}});
            logger->log(
                mf::core::LogLevel::warn,
                "slow_request trace_id=" + trace_id + " method=" +
                    std::string{mf::http::method_name(request.method())} + " path=" + normalized_target(request) +
                    " status=" + std::to_string(response.result_int()) +
                    " duration_ms=" + std::to_string(duration_ms));
        } else if (should_log_regular_request) {
            logger->log(
                mf::core::LogLevel::info,
                "request_done trace_id=" + trace_id + " method=" +
                    std::string{mf::http::method_name(request.method())} + " path=" + normalized_target(request) +
                    " status=" + std::to_string(response.result_int()) +
                    " duration_ms=" + std::to_string(duration_ms));
        }
        co_return response;
    });

    const mf::chat::GatewayOptions chat_options{
        .host = app.options().server.host,
        .port = 8090,
    };

    // 系统路由
    register_system_routes(
        app,
        repositories,
        chat_options,
        storage_options,
        metrics,
        tracer,
        load_probe,
        runtime_environment);

    // 认证路由
    register_auth_routes(app, repositories, token_service, account_service, runtime_environment);
    register_agent_routes(app, token_service, agent_service, agent_task_service);

    // 玩家侧路由
    register_player_routes(app, repositories, token_service);
    register_mail_routes(app, repositories, mail_service, token_service);
    register_friend_routes(app, repositories, chat_service, chat_state_service, token_service);
    register_chat_routes(app, repositories, chat_service, chat_state_service, token_service);

    // 运营投递路由
    register_reward_routes(app, repositories, token_service, reward_service, mail_service, admin_task_scheduler);
    register_gacha_routes(app, repositories, token_service, gacha_service);
    register_shop_routes(app, repositories, token_service, shop_service);
    register_loot_routes(app, repositories, token_service, loot_service);
    register_quest_routes(app, repositories, token_service, quest_service);
    register_multiplayer_routes(app, token_service, multiplayer_service);
    register_voice_routes(app, repositories, token_service, voice_service);

    // 后台管理路由
    register_admin_entry_routes(app, repositories, token_service, account_service, bootstrap_token);
    register_admin_user_routes(app, repositories, token_service, account_service);
    register_admin_audit_routes(app, repositories, token_service);
    register_admin_query_routes(
        app,
        repositories,
        token_service,
        account_service,
        reward_service,
        mail_service,
        chat_service);
    register_admin_chat_routes(app, repositories, token_service, chat_service);

    // 后台任务路由
    register_admin_task_routes(app, repositories, token_service, admin_task_scheduler);

        app.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hello_service startup failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "hello_service startup failed: unknown error\n";
        return 1;
    }
}
