#include "hello_admin_tasks.hpp"

#include "hello_route_support.hpp"

#include "mf/chat/channels.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"
#include "mf/game/mail_service.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value_to.hpp>

#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string admin_task_kind_name(mf::game::AdminTaskKind kind) {
    switch (kind) {
    case mf::game::AdminTaskKind::announce:
        return "announce";
    case mf::game::AdminTaskKind::batch_reward:
        return "batch_reward";
    case mf::game::AdminTaskKind::batch_mail:
        return "batch_mail";
    case mf::game::AdminTaskKind::batch_mute:
        return "batch_mute";
    case mf::game::AdminTaskKind::batch_account_status:
        return "batch_account_status";
    }
    return "unknown";
}

}  // namespace

// 文件职责：
// 1. 实现后台任务调度器本体。
// 2. 统一承载“claim 到期任务 -> 执行业务 -> 回写任务状态/审计/指标”的流程。
// 3. 让 API 进程和 worker 进程复用同一套任务编排代码，而不是各自维护一份。
//
// 文件调用流程：
// 1. API 路由通过 schedule/retry_task 写入 `admin_task` 表。
// 2. 独立 worker 进程调用 run_until_stopped() 周期性 claim 到期任务。
// 3. 每个任务执行后都会更新任务状态、记录审计、写 tracing/metrics。

struct AdminTaskScheduler::Impl {
    Impl(
        std::shared_ptr<mf::chat::ChatService> chat_service_in,
        std::shared_ptr<mf::auth::AccountService> account_service_in,
        mf::game::RepositoryBundle repositories_in,
        std::shared_ptr<mf::game::AuditLogRepository> audit_logs_in,
        std::shared_ptr<mf::game::AdminTaskRepository> task_repository_in,
        std::shared_ptr<mf::core::MetricsRegistry> metrics_in,
        std::shared_ptr<mf::core::Logger> logger_in)
        : chat_service(std::move(chat_service_in)),
          account_service(std::move(account_service_in)),
          repositories(std::move(repositories_in)),
          reward_service(std::make_shared<mf::game::RewardService>(
              repositories.players,
              repositories.rewards,
              repositories.reward_templates,
              repositories.item_catalog,
              repositories.reward_transactions)),
          mail_service(std::make_shared<mf::game::MailService>(
              repositories.mails,
              repositories.players,
              repositories.rewards,
              reward_service)),
          audit_logs(std::move(audit_logs_in)),
          task_repository(std::move(task_repository_in)),
          metrics(std::move(metrics_in)),
          logger(std::move(logger_in)) {}

    void observe_task_execution(
        mf::game::AdminTaskKind kind,
        std::string_view status,
        std::chrono::steady_clock::time_point started_at) const {
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count();
        metrics->increment_counter(
            "mf_admin_task_executions_total",
            1.0,
            {
                {"kind", admin_task_kind_name(kind)},
                {"status", std::string{status}},
            });
        metrics->observe_histogram_ms(
            "mf_admin_task_duration_ms",
            static_cast<double>(duration_ms),
            {
                {"kind", admin_task_kind_name(kind)},
                {"status", std::string{status}},
            });
        if (duration_ms >= 1000) {
            metrics->increment_counter(
                "mf_alert_events_total",
                1.0,
                {
                    {"category", "slow_admin_task"},
                    {"kind", admin_task_kind_name(kind)},
                });
            logger->log(
                mf::core::LogLevel::warn,
                "slow_admin_task kind=" + admin_task_kind_name(kind) + " status=" + std::string{status} +
                    " duration_ms=" + std::to_string(duration_ms));
        }
    }

    bool process_announcement_tasks(std::chrono::system_clock::time_point now) {
        const auto due_tasks = task_repository->claim_due_tasks(mf::game::AdminTaskKind::announce, now, 10);
        for (const auto& due_task : due_tasks) {
            const auto started_at = std::chrono::steady_clock::now();
            const auto request = parse_json_or_empty_object(due_task.request_json);
            auto execute_span = start_task_execution_span(due_task, request);
            execute_span.set_attribute("admin.operator_id", due_task.operator_id);
            execute_span.set_attribute("task.id", due_task.task_id);
            execute_span.set_attribute("task.target_id", due_task.target_id);
            std::optional<mf::game::ChatChannel> channel;
            if (request.if_contains("channel")) {
                const auto& channel_object = request.at("channel").as_object();
                const auto kind = boost::json::value_to<std::string>(channel_object.at("kind"));
                if (kind == "lobby") {
                    channel = mf::chat::make_lobby_channel(
                        channel_object.if_contains("scope_id")
                            ? boost::json::value_to<std::string>(channel_object.at("scope_id"))
                            : std::string{"global"});
                } else if (kind == "room" && channel_object.if_contains("scope_id")) {
                    channel = mf::chat::make_room_channel(
                        boost::json::value_to<std::string>(channel_object.at("scope_id")));
                }
            }
            if (!channel) {
                task_repository->mark_failed(
                    due_task.task_id,
                    "invalid scheduled channel payload",
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    std::chrono::system_clock::now());
                observe_task_execution(due_task.kind, "failed", started_at);
                continue;
            }
            const auto result = chat_service->publish(mf::game::ChatMessage{
                .channel = *channel,
                .kind = mf::game::ChatMessageKind::system,
                .sender_player_id = "gm_system",
                .sender_nickname = "GM",
                .text = request.if_contains("text") ? boost::json::value_to<std::string>(request.at("text"))
                                                    : std::string{},
                .sent_at = std::chrono::system_clock::now(),
            });

            const auto executed_at = std::chrono::system_clock::now();
            if (result.ok) {
                if (repositories.announcements) {
                    repositories.announcements->publish(
                        *channel,
                        due_task.operator_id,
                        "定时公告",
                        request.if_contains("text") ? boost::json::value_to<std::string>(request.at("text"))
                                                    : std::string{},
                        executed_at,
                        std::nullopt);
                }
                const auto updated = task_repository->mark_completed(
                    due_task.task_id,
                    boost::json::serialize(boost::json::object{{"ok", true}}),
                    executed_at);
                audit_logs->append(mf::game::AuditLogWriteRequest{
                    .operator_id = due_task.operator_id,
                    .action = "announcement_task_execute",
                    .target_type = "channel",
                    .target_id = due_task.target_id,
                    .request_ip = due_task.request_ip,
                    .user_agent = due_task.user_agent,
                    .trace_id = due_task.trace_id,
                    .success = true,
                    .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started_at)
                                       .count(),
                    .after_json = updated ? boost::json::serialize(mf::game::to_json(*updated)) : std::string{},
                    .detail_json = due_task.request_json,
                });
                observe_task_execution(due_task.kind, "completed", started_at);
            } else {
                const auto updated = task_repository->mark_failed(
                    due_task.task_id,
                    result.message,
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    executed_at);
                audit_logs->append(mf::game::AuditLogWriteRequest{
                    .operator_id = due_task.operator_id,
                    .action = "announcement_task_execute",
                    .target_type = "channel",
                    .target_id = due_task.target_id,
                    .request_ip = due_task.request_ip,
                    .user_agent = due_task.user_agent,
                    .trace_id = due_task.trace_id,
                    .success = false,
                    .failure_reason = result.message,
                    .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started_at)
                                       .count(),
                    .after_json = updated ? boost::json::serialize(mf::game::to_json(*updated)) : std::string{},
                    .detail_json = due_task.request_json,
                });
                observe_task_execution(due_task.kind, "failed", started_at);
            }
        }
        return !due_tasks.empty();
    }

    bool process_batch_mute_tasks(std::chrono::system_clock::time_point now) {
        const auto due_tasks = task_repository->claim_due_tasks(mf::game::AdminTaskKind::batch_mute, now, 10);
        for (const auto& due_task : due_tasks) {
            const auto started_at = std::chrono::steady_clock::now();
            const auto request = parse_json_or_empty_object(due_task.request_json);
            auto execute_span = start_task_execution_span(due_task, request);
            execute_span.set_attribute("admin.operator_id", due_task.operator_id);
            execute_span.set_attribute("task.id", due_task.task_id);
            const auto channel = read_channel_from_object(request);
            if (!channel) {
                task_repository->mark_failed(
                    due_task.task_id,
                    "invalid batch mute channel payload",
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    std::chrono::system_clock::now());
                observe_task_execution(due_task.kind, "failed", started_at);
                continue;
            }
            const auto player_ids = resolve_target_players(request, repositories.players);
            execute_span.set_attribute("game.player_count", std::to_string(player_ids.size()));
            boost::json::array items;
            const auto duration_seconds = read_optional_i64_field(request, "duration_seconds", 3600);
            for (const auto& player_id : player_ids) {
                const auto mute = chat_service->mute_player(
                    channel->channel_id,
                    player_id,
                    read_optional_string_field(request, "muted_by_player_id").empty()
                        ? due_task.operator_id
                        : read_optional_string_field(request, "muted_by_player_id"),
                    read_optional_string_field(request, "reason"),
                    std::chrono::system_clock::now() + std::chrono::seconds{duration_seconds});
                items.push_back(boost::json::object{
                    {"player_id", player_id},
                    {"success", true},
                    {"mute", mf::game::to_json(mute)},
                });
            }
            const auto result_json = boost::json::serialize(boost::json::object{
                {"channel_id", channel->channel_id},
                {"player_count", static_cast<std::int64_t>(player_ids.size())},
                {"success_count", static_cast<std::int64_t>(player_ids.size())},
                {"failure_count", 0},
                {"items", items},
            });
            const auto updated = task_repository->mark_completed(
                due_task.task_id,
                result_json,
                std::chrono::system_clock::now());
            audit_logs->append(mf::game::AuditLogWriteRequest{
                .operator_id = due_task.operator_id,
                .action = "batch_mute_task_execute",
                .target_type = "channel",
                .target_id = due_task.target_id,
                .request_ip = due_task.request_ip,
                .user_agent = due_task.user_agent,
                .trace_id = due_task.trace_id,
                .success = true,
                .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count(),
                .after_json = updated ? boost::json::serialize(mf::game::to_json(*updated)) : std::string{},
                .detail_json = due_task.request_json,
            });
            observe_task_execution(due_task.kind, "completed", started_at);
        }
        return !due_tasks.empty();
    }

    bool process_batch_account_status_tasks(std::chrono::system_clock::time_point now) {
        const auto due_tasks =
            task_repository->claim_due_tasks(mf::game::AdminTaskKind::batch_account_status, now, 10);
        for (const auto& due_task : due_tasks) {
            const auto started_at = std::chrono::steady_clock::now();
            const auto request = parse_json_or_empty_object(due_task.request_json);
            auto execute_span = start_task_execution_span(due_task, request);
            execute_span.set_attribute("admin.operator_id", due_task.operator_id);
            execute_span.set_attribute("task.id", due_task.task_id);
            const auto status = parse_account_status(read_string_field(request, "status"));
            if (!status) {
                task_repository->mark_failed(
                    due_task.task_id,
                    "invalid batch account status payload",
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    std::chrono::system_clock::now());
                observe_task_execution(due_task.kind, "failed", started_at);
                continue;
            }
            const auto player_ids = resolve_target_players(request, repositories.players);
            execute_span.set_attribute("game.player_count", std::to_string(player_ids.size()));
            boost::json::array items;
            std::size_t success_count = 0;
            for (const auto& player_id : player_ids) {
                const auto profile = repositories.players->find_by_player_id(player_id);
                if (!profile) {
                    items.push_back(boost::json::object{
                        {"player_id", player_id},
                        {"success", false},
                        {"error_code", "player_not_found"},
                    });
                    continue;
                }
                const auto result = account_service->set_account_status(
                    profile->account_id,
                    *status,
                    read_optional_string_field(request, "reason"));
                if (result.error) {
                    items.push_back(boost::json::object{
                        {"player_id", player_id},
                        {"account_id", profile->account_id},
                        {"success", false},
                        {"error_code", result.error->code},
                    });
                    continue;
                }
                ++success_count;
                items.push_back(boost::json::object{
                    {"player_id", player_id},
                    {"account_id", profile->account_id},
                    {"success", true},
                    {"account", mf::game::to_json(*result.value)},
                });
            }
            const auto result_json = boost::json::serialize(boost::json::object{
                {"player_count", static_cast<std::int64_t>(player_ids.size())},
                {"success_count", static_cast<std::int64_t>(success_count)},
                {"failure_count", static_cast<std::int64_t>(player_ids.size() - success_count)},
                {"items", items},
            });
            const auto updated = task_repository->mark_completed(
                due_task.task_id,
                result_json,
                std::chrono::system_clock::now());
            audit_logs->append(mf::game::AuditLogWriteRequest{
                .operator_id = due_task.operator_id,
                .action = "batch_account_status_task_execute",
                .target_type = "player_batch",
                .target_id = due_task.target_id,
                .request_ip = due_task.request_ip,
                .user_agent = due_task.user_agent,
                .trace_id = due_task.trace_id,
                .success = true,
                .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count(),
                .after_json = updated ? boost::json::serialize(mf::game::to_json(*updated)) : std::string{},
                .detail_json = due_task.request_json,
            });
            observe_task_execution(
                due_task.kind,
                success_count == player_ids.size() ? "completed" : "partial_failure",
                started_at);
        }
        return !due_tasks.empty();
    }

    bool process_batch_reward_tasks(std::chrono::system_clock::time_point now) {
        const auto due_tasks = task_repository->claim_due_tasks(mf::game::AdminTaskKind::batch_reward, now, 10);
        for (const auto& due_task : due_tasks) {
            const auto started_at = std::chrono::steady_clock::now();
            const auto request = parse_json_or_empty_object(due_task.request_json);
            auto execute_span = start_task_execution_span(due_task, request);
            execute_span.set_attribute("admin.operator_id", due_task.operator_id);
            execute_span.set_attribute("task.id", due_task.task_id);

            std::string reward_error_message;
            const auto rewards = read_rewards_array(request, reward_error_message);
            if (!rewards) {
                task_repository->mark_failed(
                    due_task.task_id,
                    reward_error_message.empty() ? "invalid batch reward payload" : reward_error_message,
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    std::chrono::system_clock::now());
                observe_task_execution(due_task.kind, "failed", started_at);
                continue;
            }

            const auto player_ids = resolve_target_players(request, repositories.players);
            execute_span.set_attribute("game.player_count", std::to_string(player_ids.size()));
            const auto idempotency_scope = request.if_contains("retry_of_task_id")
                                               ? read_string_field(request, "retry_of_task_id")
                                               : due_task.task_id;
            const auto source_type = read_optional_string_field(request, "source_type").empty()
                                         ? std::string{"admin_batch"}
                                         : read_optional_string_field(request, "source_type");
            const auto source_id = read_optional_string_field(request, "source_id");
            boost::json::array items;
            std::size_t success_count = 0;
            for (const auto& player_id : player_ids) {
                const auto result = reward_service->grant_to_player(
                    player_id,
                    "batch_grant:" + player_id + ":" + idempotency_scope,
                    source_type,
                    source_id,
                    *rewards);
                if (result.error) {
                    items.push_back(boost::json::object{
                        {"player_id", player_id},
                        {"success", false},
                        {"error_code", result.error->code},
                    });
                    continue;
                }
                ++success_count;
                items.push_back(boost::json::object{
                    {"player_id", player_id},
                    {"success", true},
                    {"grant", mf::game::to_json(*result.value)},
                });
            }
            const auto result_json = boost::json::serialize(boost::json::object{
                {"player_count", static_cast<std::int64_t>(player_ids.size())},
                {"success_count", static_cast<std::int64_t>(success_count)},
                {"failure_count", static_cast<std::int64_t>(player_ids.size() - success_count)},
                {"items", items},
            });
            const auto updated = task_repository->mark_completed(
                due_task.task_id,
                result_json,
                std::chrono::system_clock::now());
            audit_logs->append(mf::game::AuditLogWriteRequest{
                .operator_id = due_task.operator_id,
                .action = "batch_reward_task_execute",
                .target_type = "player_batch",
                .target_id = due_task.target_id,
                .request_ip = due_task.request_ip,
                .user_agent = due_task.user_agent,
                .trace_id = due_task.trace_id,
                .success = true,
                .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count(),
                .after_json = updated ? boost::json::serialize(mf::game::to_json(*updated)) : std::string{},
                .detail_json = due_task.request_json,
            });
            observe_task_execution(
                due_task.kind,
                success_count == player_ids.size() ? "completed" : "partial_failure",
                started_at);
        }
        return !due_tasks.empty();
    }

    bool process_batch_mail_tasks(std::chrono::system_clock::time_point now) {
        const auto due_tasks = task_repository->claim_due_tasks(mf::game::AdminTaskKind::batch_mail, now, 10);
        for (const auto& due_task : due_tasks) {
            const auto started_at = std::chrono::steady_clock::now();
            const auto request = parse_json_or_empty_object(due_task.request_json);
            auto execute_span = start_task_execution_span(due_task, request);
            execute_span.set_attribute("admin.operator_id", due_task.operator_id);
            execute_span.set_attribute("task.id", due_task.task_id);

            const auto title = read_string_field(request, "title");
            const auto content = read_string_field(request, "body");
            if (title.empty() || content.empty()) {
                task_repository->mark_failed(
                    due_task.task_id,
                    "mail title and body are required",
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    std::chrono::system_clock::now());
                observe_task_execution(due_task.kind, "failed", started_at);
                continue;
            }

            std::string reward_error_message;
            const auto rewards = read_rewards_array(request, reward_error_message);
            if (!rewards) {
                task_repository->mark_failed(
                    due_task.task_id,
                    reward_error_message.empty() ? "invalid batch mail rewards" : reward_error_message,
                    boost::json::serialize(boost::json::object{{"ok", false}}),
                    std::chrono::system_clock::now());
                observe_task_execution(due_task.kind, "failed", started_at);
                continue;
            }

            std::optional<std::chrono::system_clock::time_point> expires_at;
            if (request.if_contains("expires_in_seconds")) {
                expires_at = std::chrono::system_clock::now() +
                    std::chrono::seconds{read_optional_i64_field(request, "expires_in_seconds", 0)};
            }
            const auto player_ids = resolve_target_players(request, repositories.players);
            execute_span.set_attribute("game.player_count", std::to_string(player_ids.size()));
            const auto sender_name = read_optional_string_field(request, "sender_name").empty()
                                         ? std::string{"System"}
                                         : read_optional_string_field(request, "sender_name");
            const auto mails = mail_service->send_batch(
                player_ids,
                sender_name,
                title,
                content,
                *rewards,
                expires_at);
            const auto result_json = boost::json::serialize(boost::json::object{
                {"player_count", static_cast<std::int64_t>(player_ids.size())},
                {"mail_count", static_cast<std::int64_t>(mails.size())},
            });
            const auto updated = task_repository->mark_completed(
                due_task.task_id,
                result_json,
                std::chrono::system_clock::now());
            audit_logs->append(mf::game::AuditLogWriteRequest{
                .operator_id = due_task.operator_id,
                .action = "batch_mail_task_execute",
                .target_type = "player_batch",
                .target_id = due_task.target_id,
                .request_ip = due_task.request_ip,
                .user_agent = due_task.user_agent,
                .trace_id = due_task.trace_id,
                .success = true,
                .duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count(),
                .after_json = updated ? boost::json::serialize(mf::game::to_json(*updated)) : std::string{},
                .detail_json = due_task.request_json,
            });
            observe_task_execution(due_task.kind, "completed", started_at);
        }
        return !due_tasks.empty();
    }

    mf::core::SpanScope start_task_execution_span(
        const mf::game::AdminTask& task,
        const boost::json::object& request) const {
        const auto tracer = mf::core::global_tracer();
        if (!tracer) {
            return {};
        }
        std::optional<mf::core::TraceContext> parent_context;
        if (const auto* value = request.if_contains("_traceparent")) {
            parent_context = mf::core::TracerProvider::parse_traceparent(boost::json::value_to<std::string>(*value));
        }
        auto span = parent_context
            ? tracer->start_span(
                  "admin.task.execute",
                  {
                      {"task.kind", admin_task_kind_name(task.kind)},
                      {"task.id", task.task_id},
                      {"task.status", task.status},
                      {"task.target_type", task.target_type},
                      {"task.target_id", task.target_id},
                  },
                  parent_context)
            : tracer->start_span_with_trace(
                  "admin.task.execute",
                  task.trace_id,
                  std::nullopt,
                  {
                      {"task.kind", admin_task_kind_name(task.kind)},
                      {"task.id", task.task_id},
                      {"task.status", task.status},
                      {"task.target_type", task.target_type},
                      {"task.target_id", task.target_id},
                  });
        if (const auto* value = request.if_contains("retry_of_task_id")) {
            span.set_attribute("task.retry_of_task_id", boost::json::value_to<std::string>(*value));
        }
        if (const auto* value = request.if_contains("player_ids")) {
            span.set_attribute("game.player_count", std::to_string(value->as_array().size()));
        }
        return span;
    }

    std::shared_ptr<mf::chat::ChatService> chat_service;
    std::shared_ptr<mf::auth::AccountService> account_service;
    mf::game::RepositoryBundle repositories;
    std::shared_ptr<mf::game::RewardService> reward_service;
    std::shared_ptr<mf::game::MailService> mail_service;
    std::shared_ptr<mf::game::AuditLogRepository> audit_logs;
    std::shared_ptr<mf::game::AdminTaskRepository> task_repository;
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    std::shared_ptr<mf::core::Logger> logger;
};

AdminTaskScheduler::AdminTaskScheduler(
    std::shared_ptr<mf::chat::ChatService> chat_service,
    std::shared_ptr<mf::auth::AccountService> account_service,
    mf::game::RepositoryBundle repositories,
    std::shared_ptr<mf::game::AuditLogRepository> audit_logs,
    std::shared_ptr<mf::game::AdminTaskRepository> task_repository,
    std::shared_ptr<mf::core::MetricsRegistry> metrics,
    std::shared_ptr<mf::core::Logger> logger)
    : impl_(std::make_unique<Impl>(
          std::move(chat_service),
          std::move(account_service),
          std::move(repositories),
          std::move(audit_logs),
          std::move(task_repository),
          std::move(metrics),
          std::move(logger))) {}

AdminTaskScheduler::~AdminTaskScheduler() = default;

mf::game::AdminTask AdminTaskScheduler::schedule(
    mf::game::ChatChannel channel,
    std::string text,
    std::string operator_id,
    std::string request_ip,
    std::string user_agent,
    std::string trace_id,
    std::chrono::system_clock::time_point execute_at) {
    boost::json::object request_object{
        {"channel", mf::game::to_json(channel)},
        {"text", text},
    };
    if (const auto tracer = mf::core::global_tracer()) {
        const auto traceparent = tracer->current_traceparent();
        if (!traceparent.empty()) {
            request_object["_traceparent"] = traceparent;
        }
    }
    return impl_->task_repository->save(mf::game::AdminTask{
        .kind = mf::game::AdminTaskKind::announce,
        .status = "scheduled",
        .operator_id = std::move(operator_id),
        .target_type = "channel",
        .target_id = channel.channel_id,
        .request_ip = std::move(request_ip),
        .user_agent = std::move(user_agent),
        .trace_id = std::move(trace_id),
        .request_json = boost::json::serialize(std::move(request_object)),
        .execute_at = execute_at,
    });
}

mf::game::AdminTask AdminTaskScheduler::schedule_batch_mute(
    boost::json::object request_object,
    std::string operator_id,
    std::string request_ip,
    std::string user_agent,
    std::string trace_id) {
    if (const auto tracer = mf::core::global_tracer()) {
        const auto traceparent = tracer->current_traceparent();
        if (!traceparent.empty()) {
            request_object["_traceparent"] = traceparent;
        }
    }
    return impl_->task_repository->save(mf::game::AdminTask{
        .kind = mf::game::AdminTaskKind::batch_mute,
        .status = "scheduled",
        .operator_id = std::move(operator_id),
        .target_type = "player_batch",
        .target_id = request_object.if_contains("player_ids")
                         ? std::to_string(request_object.at("player_ids").as_array().size())
                         : std::string{"0"},
        .request_ip = std::move(request_ip),
        .user_agent = std::move(user_agent),
        .trace_id = std::move(trace_id),
        .request_json = boost::json::serialize(request_object),
        .execute_at = std::chrono::system_clock::now(),
    });
}

mf::game::AdminTask AdminTaskScheduler::schedule_batch_account_status(
    boost::json::object request_object,
    std::string operator_id,
    std::string request_ip,
    std::string user_agent,
    std::string trace_id) {
    if (const auto tracer = mf::core::global_tracer()) {
        const auto traceparent = tracer->current_traceparent();
        if (!traceparent.empty()) {
            request_object["_traceparent"] = traceparent;
        }
    }
    return impl_->task_repository->save(mf::game::AdminTask{
        .kind = mf::game::AdminTaskKind::batch_account_status,
        .status = "scheduled",
        .operator_id = std::move(operator_id),
        .target_type = "player_batch",
        .target_id = request_object.if_contains("player_ids")
                         ? std::to_string(request_object.at("player_ids").as_array().size())
                         : std::string{"0"},
        .request_ip = std::move(request_ip),
        .user_agent = std::move(user_agent),
        .trace_id = std::move(trace_id),
        .request_json = boost::json::serialize(request_object),
        .execute_at = std::chrono::system_clock::now(),
    });
}

mf::game::AdminTask AdminTaskScheduler::schedule_batch_reward(
    boost::json::object request_object,
    std::string operator_id,
    std::string request_ip,
    std::string user_agent,
    std::string trace_id) {
    if (const auto tracer = mf::core::global_tracer()) {
        const auto traceparent = tracer->current_traceparent();
        if (!traceparent.empty()) {
            request_object["_traceparent"] = traceparent;
        }
    }
    return impl_->task_repository->save(mf::game::AdminTask{
        .kind = mf::game::AdminTaskKind::batch_reward,
        .status = "scheduled",
        .operator_id = std::move(operator_id),
        .target_type = "player_batch",
        .target_id = request_object.if_contains("player_ids")
                         ? std::to_string(request_object.at("player_ids").as_array().size())
                         : std::string{"0"},
        .request_ip = std::move(request_ip),
        .user_agent = std::move(user_agent),
        .trace_id = std::move(trace_id),
        .request_json = boost::json::serialize(request_object),
        .execute_at = std::chrono::system_clock::now(),
    });
}

mf::game::AdminTask AdminTaskScheduler::schedule_batch_mail(
    boost::json::object request_object,
    std::string operator_id,
    std::string request_ip,
    std::string user_agent,
    std::string trace_id) {
    if (const auto tracer = mf::core::global_tracer()) {
        const auto traceparent = tracer->current_traceparent();
        if (!traceparent.empty()) {
            request_object["_traceparent"] = traceparent;
        }
    }
    return impl_->task_repository->save(mf::game::AdminTask{
        .kind = mf::game::AdminTaskKind::batch_mail,
        .status = "scheduled",
        .operator_id = std::move(operator_id),
        .target_type = "player_batch",
        .target_id = request_object.if_contains("player_ids")
                         ? std::to_string(request_object.at("player_ids").as_array().size())
                         : std::string{"0"},
        .request_ip = std::move(request_ip),
        .user_agent = std::move(user_agent),
        .trace_id = std::move(trace_id),
        .request_json = boost::json::serialize(request_object),
        .execute_at = std::chrono::system_clock::now(),
    });
}

std::optional<mf::game::AdminTask> AdminTaskScheduler::retry_task(
    const mf::game::AdminTask& original_task,
    bool failed_only,
    std::string operator_id,
    std::string request_ip,
    std::string user_agent,
    std::string trace_id) {
    auto request_object = parse_json_or_empty_object(original_task.request_json);
    if (failed_only) {
        const auto result_object = parse_json_or_empty_object(original_task.result_json);
        std::vector<std::string> failed_player_ids;
        if (const auto* items = result_object.if_contains("items")) {
            for (const auto& item_value : items->as_array()) {
                const auto& item = item_value.as_object();
                if (item.if_contains("success") && !item.at("success").as_bool() && item.if_contains("player_id")) {
                    failed_player_ids.push_back(boost::json::value_to<std::string>(item.at("player_id")));
                }
            }
        }
        if (failed_player_ids.empty() &&
            (original_task.kind == mf::game::AdminTaskKind::batch_account_status ||
             original_task.kind == mf::game::AdminTaskKind::batch_mute)) {
            return std::nullopt;
        }
        if (!failed_player_ids.empty()) {
            boost::json::array player_ids;
            for (const auto& player_id : failed_player_ids) {
                player_ids.emplace_back(player_id);
            }
            request_object["player_ids"] = std::move(player_ids);
        }
    }

    request_object["retry_of_task_id"] = original_task.task_id;
    request_object["retry_trace_id"] = trace_id;
    switch (original_task.kind) {
    case mf::game::AdminTaskKind::announce: {
        const auto channel = read_channel_from_object(request_object);
        if (!channel) {
            return std::nullopt;
        }
        return schedule(
            *channel,
            read_string_field(request_object, "text"),
            std::move(operator_id),
            std::move(request_ip),
            std::move(user_agent),
            std::move(trace_id),
            std::chrono::system_clock::now());
    }
    case mf::game::AdminTaskKind::batch_mute:
        return schedule_batch_mute(
            std::move(request_object),
            std::move(operator_id),
            std::move(request_ip),
            std::move(user_agent),
            std::move(trace_id));
    case mf::game::AdminTaskKind::batch_account_status:
        return schedule_batch_account_status(
            std::move(request_object),
            std::move(operator_id),
            std::move(request_ip),
            std::move(user_agent),
            std::move(trace_id));
    case mf::game::AdminTaskKind::batch_reward:
        return schedule_batch_reward(
            std::move(request_object),
            std::move(operator_id),
            std::move(request_ip),
            std::move(user_agent),
            std::move(trace_id));
    case mf::game::AdminTaskKind::batch_mail:
        return schedule_batch_mail(
            std::move(request_object),
            std::move(operator_id),
            std::move(request_ip),
            std::move(user_agent),
            std::move(trace_id));
    }
    return std::nullopt;
}

void AdminTaskScheduler::run_until_stopped(std::stop_token stop_token) {
    impl_->logger->log(mf::core::LogLevel::info, "admin task worker loop started");
    while (!stop_token.stop_requested()) {
        const auto now = std::chrono::system_clock::now();
        auto processed_any = false;
        processed_any = impl_->process_announcement_tasks(now) || processed_any;
        processed_any = impl_->process_batch_mute_tasks(now) || processed_any;
        processed_any = impl_->process_batch_account_status_tasks(now) || processed_any;
        processed_any = impl_->process_batch_reward_tasks(now) || processed_any;
        processed_any = impl_->process_batch_mail_tasks(now) || processed_any;
        if (!processed_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
    }
    impl_->logger->log(mf::core::LogLevel::info, "admin task worker loop stopped");
}

std::shared_ptr<AdminTaskScheduler> make_admin_task_scheduler(
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::game::AuditLogRepository>& audit_logs,
    const std::shared_ptr<mf::game::AdminTaskRepository>& task_repository,
    const std::shared_ptr<mf::core::MetricsRegistry>& metrics,
    const std::shared_ptr<mf::core::Logger>& logger) {
    return std::make_shared<AdminTaskScheduler>(
        chat_service,
        account_service,
        repositories,
        audit_logs,
        task_repository,
        metrics,
        logger);
}
