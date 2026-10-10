#include "admin_ui_page.hpp"

#include "mf/chat/channels.hpp"
#include "mf/chat/chat_service.hpp"
#include "mf/chat/gateway.hpp"
#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/env.hpp"
#include "mf/core/metrics.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"
#include "mf/http/query_params.hpp"
#include "mf/storage/options.hpp"
#include "mf/storage/redis_repositories.hpp"
#include "mf/storage/review.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <chrono>
#include <thread>

// 本文件职责：
// 1. 承接系统级只读路由。
// 2. 只保留不依赖复杂业务服务的健康检查和配置观察接口。
// 3. 作为整个 hello 示例里最轻量的一层，方便先 review 系统入口。
//
// 文件调用流程：
// 1. main.cpp 构造 Application、GatewayOptions、StorageOptions。
// 2. main.cpp 调用 register_system_routes(...)。
// 3. 这里把 /health 和 /config 挂到 Application。
// 4. 请求进入后不经过业务仓储，只读取应用或配置对象。
// 5. 最后直接返回系统状态或当前配置快照。

// 注册系统级只读路由。
// 这组接口没有业务副作用，适合作为最先抽离的轻量分组。
void register_system_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const mf::chat::GatewayOptions& chat_options,
    const mf::storage::StorageOptions& storage_options,
    const std::shared_ptr<mf::core::MetricsRegistry>& metrics,
    const std::shared_ptr<mf::core::TracerProvider>& tracer,
    const std::shared_ptr<mf::http::LoadProbe>& load_probe,
    mf::core::RuntimeEnvironment runtime_environment) {
    const auto config_snapshot = boost::json::object{
        {"name", app.options().name},
        {"runtime_environment", mf::core::to_string(runtime_environment)},
        {"host", app.options().server.host.to_string()},
        {"port", app.options().server.port},
        {"thread_count", app.options().server.thread_count},
        {"accept_concurrency", app.options().server.accept_concurrency},
        {"business_thread_count", app.options().server.business_thread_count},
        {"max_inflight_requests", static_cast<std::int64_t>(app.options().server.max_inflight_requests)},
        {"max_pending_business_tasks", static_cast<std::int64_t>(app.options().server.max_pending_business_tasks)},
        {"slow_request_threshold_ms", static_cast<std::int64_t>(app.options().server.slow_request_threshold_ms)},
        {"chat_port", chat_options.port},
        {"storage_mode", mf::storage::to_string(storage_options.mode)},
        {"storage_notes", std::string(mf::storage::mysql_redis_review_notes())},
        {"mysql_host", storage_options.mysql.host},
        {"mysql_port", storage_options.mysql.port},
        {"redis_host", storage_options.redis.host},
        {"redis_port", storage_options.redis.port},
    };

    const auto health_handler = [runtime_environment](const mf::http::Request& request) {
        // 压测时可用 /health?sleep_ms=50 或 /health/delay?sleep_ms=50 模拟同步业务耗时，
        // 方便观察业务线程池积压、429/503 背压点和慢请求指标。
        const auto sleep_ms = std::min<std::size_t>(
            5000,
            mf::http::read_query_size(request.target(), "sleep_ms", 0));
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        return mf::core::make_ok_response(
            {
                {"status", "ok"},
                {"transport", "http"},
                {"runtime_environment", mf::core::to_string(runtime_environment)},
                {"sleep_ms", static_cast<std::int64_t>(sleep_ms)},
            },
            request.version());
    };

    app.route(
        mf::http::beast_http::verb::get,
        "/admin",
        [](const mf::http::Request& request) {
            return mf::http::make_html_response(
                mf::http::beast_http::status::ok,
                std::string{hello_admin_ui_page_html()},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/ui",
        [](const mf::http::Request& request) {
            return mf::http::make_html_response(
                mf::http::beast_http::status::ok,
                std::string{hello_admin_ui_page_html()},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/health",
        health_handler);

    app.route(
        mf::http::beast_http::verb::get,
        "/health/delay",
        health_handler);

    app.route(
        mf::http::beast_http::verb::get,
        "/metrics",
        [metrics](const mf::http::Request& request) {
            mf::http::Response response{mf::http::beast_http::status::ok, request.version()};
            response.set(mf::http::beast_http::field::content_type, "text/plain; version=0.0.4");
            response.body() = metrics ? metrics->render_prometheus() : std::string{};
            response.prepare_payload();
            return response;
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/debug/traces",
        [tracer](const mf::http::Request& request) {
            boost::json::array items;
            for (const auto& span : tracer->recent_spans(100)) {
                boost::json::array attributes;
                for (const auto& attribute : span.attributes) {
                    attributes.push_back(boost::json::object{
                        {"key", attribute.key},
                        {"value", attribute.value},
                    });
                }
                items.push_back(boost::json::object{
                    {"name", span.name},
                    {"trace_id", span.context.trace_id},
                    {"span_id", span.context.span_id},
                    {"parent_span_id", span.parent_span_id.value_or(std::string{})},
                    {"traceparent", mf::core::TracerProvider::format_traceparent(span.context)},
                    {"status", span.status},
                    {"started_at", static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(span.started_at.time_since_epoch()).count())},
                    {"ended_at", static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(span.ended_at.time_since_epoch()).count())},
                    {"attributes", attributes},
                });
            }
            return mf::core::make_ok_response(boost::json::object{{"items", items}}, request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/debug/load",
        [load_probe](const mf::http::Request& request) {
            const auto snapshot = load_probe ? load_probe->snapshot() : mf::http::LoadSnapshot{};
            return mf::core::make_ok_response(
                boost::json::object{
                    {"inflight_requests", static_cast<std::int64_t>(snapshot.inflight_requests)},
                    {"pending_business_tasks", static_cast<std::int64_t>(snapshot.pending_business_tasks)},
                    {"max_inflight_requests", static_cast<std::int64_t>(snapshot.max_inflight_requests)},
                    {"max_pending_business_tasks", static_cast<std::int64_t>(snapshot.max_pending_business_tasks)},
                    {"rejected_too_many_inflight", static_cast<std::int64_t>(snapshot.rejected_too_many_inflight)},
                    {"rejected_business_queue_full", static_cast<std::int64_t>(snapshot.rejected_business_queue_full)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/debug/announcement-cache",
        [storage_options](const mf::http::Request& request) {
            const auto kind_param = mf::http::query_param(request.target(), "channel_kind").value_or("lobby");
            const auto scope_id = mf::http::query_param(request.target(), "scope_id").value_or("global");
            const auto limit = std::min<std::size_t>(50, mf::http::read_query_size(request.target(), "limit", 20));
            const auto kind = mf::chat::parse_channel_kind(kind_param);
            if (!kind) {
                return mf::core::make_error_response(
                    {
                        .status = mf::http::beast_http::status::bad_request,
                        .code = "channel_kind_invalid",
                        .message = "channel_kind 只能是 lobby、room 或 friend_direct",
                    },
                    request.version());
            }

            if (storage_options.mode != mf::storage::StorageMode::mysql_redis) {
                return mf::core::make_ok_response(
                    boost::json::object{
                        {"storage_mode", mf::storage::to_string(storage_options.mode)},
                        {"cache_enabled", false},
                        {"reason", "announcement redis cache only exists in mysql_redis mode"},
                    },
                    request.version());
            }

            const auto snapshot =
                mf::storage::inspect_announcement_cache(storage_options.redis, *kind, scope_id, limit);
            if (!snapshot) {
                return mf::core::make_error_response(
                    {
                        .status = mf::http::beast_http::status::bad_request,
                        .code = "scope_id_required",
                        .message = "必须提供 scope_id",
                    },
                    request.version());
            }

            return mf::core::make_ok_response(
                boost::json::object{
                    {"storage_mode", mf::storage::to_string(storage_options.mode)},
                    {"cache_enabled", true},
                    {"channel_kind", snapshot->channel_kind},
                    {"scope_id", snapshot->scope_id},
                    {"recent_key", snapshot->recent_key},
                    {"warm_key", snapshot->warm_key},
                    {"warm", snapshot->warm},
                    {"item_count", static_cast<std::int64_t>(snapshot->items.size())},
                    {"items", mf::game::to_json_array(snapshot->items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/config",
        [config_snapshot](const mf::http::Request& request) {
            // /config 只反映进程启动时就确定的只读配置，
            // 所以这里直接返回预先拼好的静态快照，避免每个请求重复构造 JSON。
            return mf::core::make_ok_response(config_snapshot, request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/system/announcements",
        [repositories](const mf::http::Request& request) {
            const auto limit = std::min<std::size_t>(50, mf::http::read_query_size(request.target(), "limit", 20));
            const auto kind_param = mf::http::query_param(request.target(), "channel_kind");
            std::optional<mf::game::ChatChannelKind> kind_filter;
            if (kind_param) {
                kind_filter = mf::chat::parse_channel_kind(*kind_param);
            }
            const auto scope_filter = mf::http::query_param(request.target(), "scope_id");
            const auto items = repositories.announcements
                ? repositories.announcements->list_recent(
                      limit,
                      kind_filter,
                      scope_filter,
                      std::chrono::system_clock::now())
                : std::vector<mf::game::AnnouncementEntry>{};
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(items.size())},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });
}
