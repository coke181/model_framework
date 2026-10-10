#include "hello_admin_tasks.hpp"
#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/game/json.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value_to.hpp>

#include <chrono>
#include <optional>

using mf::http::query_param;
using mf::http::read_query_size;

// 本文件职责：
// 1. 承接后台任务和定时公告相关路由。
// 2. 提供后台任务的 HTTP 调度、查询、重试入口。
// 3. 真正的任务执行逻辑已经拆到 hello_admin_tasks.cpp，由独立 worker 进程复用。
//
// 文件调用流程：
// 1. main.cpp 构造 repositories / token_service / scheduler。
// 2. main.cpp 调用 register_admin_task_routes(...) 挂载后台任务接口。
// 3. 管理员请求进入后先做权限校验，再调用 scheduler 写入任务或重试任务。
// 4. 独立 worker 进程随后会从任务仓储 claim 并执行这些任务。

void register_admin_task_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<AdminTaskScheduler>& admin_task_scheduler) {
    app.route(
        mf::http::beast_http::verb::post,
        "/admin/tasks/announcements",
        [repositories, token_service, admin_task_scheduler](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.task_announce",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto channel = read_channel_from_object(body->as_object());
            if (!channel) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_channel",
                        .message = "频道参数不合法",
                    },
                    request.version());
            }
            const auto execute_at = std::chrono::system_clock::now() +
                std::chrono::seconds{read_optional_i64_field(body->as_object(), "execute_in_seconds", 0)};
            const auto task = admin_task_scheduler->schedule(
                *channel,
                read_string_field(body->as_object(), "text"),
                admin->operator_name,
                client_ip_from_request(request),
                user_agent_from_request(request),
                trace_id_from_request(request),
                execute_at);
            mf::core::add_current_span_attribute("task.id", task.task_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "announcement_task_schedule",
                "channel",
                channel->channel_id,
                boost::json::serialize(body->as_object()),
                true,
                {},
                {},
                boost::json::serialize(mf::game::to_json(task)));
            return mf::core::make_ok_response(
                mf::game::to_json(task),
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/tasks/batch-mute",
        [repositories, token_service, admin_task_scheduler](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.chat_mute",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto channel = read_channel_from_object(body->as_object());
            if (!channel) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_channel",
                        .message = "频道参数不合法",
                    },
                    request.version());
            }
            const auto task = admin_task_scheduler->schedule_batch_mute(
                body->as_object(),
                admin->operator_name,
                client_ip_from_request(request),
                user_agent_from_request(request),
                trace_id_from_request(request));
            mf::core::add_current_span_attribute("task.id", task.task_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "batch_mute_task_schedule",
                "channel",
                channel->channel_id,
                boost::json::serialize(body->as_object()),
                true,
                {},
                {},
                boost::json::serialize(mf::game::to_json(task)));
            return mf::core::make_ok_response(
                mf::game::to_json(task),
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/tasks/account-status",
        [repositories, token_service, admin_task_scheduler](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.account_status",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto status = parse_account_status(read_string_field(body->as_object(), "status"));
            if (!status) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_account_status",
                        .message = "账号状态只能是 active、frozen 或 banned",
                    },
                    request.version());
            }
            const auto task = admin_task_scheduler->schedule_batch_account_status(
                body->as_object(),
                admin->operator_name,
                client_ip_from_request(request),
                user_agent_from_request(request),
                trace_id_from_request(request));
            mf::core::add_current_span_attribute("task.id", task.task_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "account_status_task_schedule",
                "player_batch",
                task.target_id,
                boost::json::serialize(body->as_object()),
                true,
                {},
                {},
                boost::json::serialize(mf::game::to_json(task)));
            return mf::core::make_ok_response(
                mf::game::to_json(task),
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/tasks/announcements",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.task_announce",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto limit = read_query_size(request.target(), "limit", 50);
            std::optional<mf::game::AdminTaskCursor> cursor;
            const auto last_created_at = query_param(request.target(), "last_created_at");
            const auto last_task_id = query_param(request.target(), "last_task_id");
            if (last_task_id) {
                try {
                    cursor = mf::game::AdminTaskCursor{
                        .last_created_at = last_created_at ? std::stoll(*last_created_at) : 0,
                        .last_task_id = *last_task_id,
                    };
                } catch (const std::exception&) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_task_cursor",
                            .message = "last_created_at 必须是合法整数",
                        },
                        request.version());
                }
            }
            const auto status = query_param(request.target(), "status");
            const auto kind = std::optional<mf::game::AdminTaskKind>{mf::game::AdminTaskKind::announce};
            const auto total = repositories.admin_tasks->count_filtered(kind, status);
            const auto page = repositories.admin_tasks->list_recent(limit, cursor, kind, status);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(page.items)},
                    {"has_more", page.has_more},
                    {"next_created_at", page.next_created_at},
                    {"next_task_id", page.next_task_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/tasks/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.task_announce",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            std::optional<mf::game::AdminTaskKind> kind;
            if (const auto* value = body->as_object().if_contains("kind")) {
                kind = parse_admin_task_kind_local(boost::json::value_to<std::string>(*value));
            }
            std::optional<std::string> status;
            if (const auto* value = body->as_object().if_contains("status")) {
                status = boost::json::value_to<std::string>(*value);
            }
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 50));
            std::optional<mf::game::AdminTaskCursor> cursor;
            const auto last_task_id = read_optional_string_field(body->as_object(), "last_task_id");
            if (!last_task_id.empty()) {
                cursor = mf::game::AdminTaskCursor{
                    .last_created_at = read_optional_i64_field(body->as_object(), "last_created_at", 0),
                    .last_task_id = last_task_id,
                };
            }
            const auto total = repositories.admin_tasks->count_filtered(kind, status);
            const auto page = repositories.admin_tasks->list_recent(limit, cursor, kind, status);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(page.items)},
                    {"has_more", page.has_more},
                    {"next_created_at", page.next_created_at},
                    {"next_task_id", page.next_task_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/tasks/retry",
        [repositories, token_service, admin_task_scheduler](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.task_announce",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto task = repositories.admin_tasks->find_by_id(read_string_field(body->as_object(), "task_id"));
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (!task) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "task_not_found",
                        .message = "后台任务不存在",
                    },
                    request.version());
            }

            const auto failed_only = read_optional_bool_field(body->as_object(), "failed_only", true);
            mf::core::add_current_span_attribute("task.id", task->task_id);
            mf::core::add_current_span_attribute("task.failed_only", failed_only ? "true" : "false");
            const auto retried_task = admin_task_scheduler->retry_task(
                *task,
                failed_only,
                admin->operator_name,
                client_ip_from_request(request),
                user_agent_from_request(request),
                trace_id_from_request(request));
            if (!retried_task) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "task_retry_not_supported",
                        .message = "当前任务类型或重试参数不支持自动重试",
                    },
                    request.version());
            }
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                failed_only ? "admin_task_retry_failed_only" : "admin_task_retry_all",
                "admin_task",
                task->task_id,
                boost::json::serialize(body->as_object()),
                true,
                {},
                boost::json::serialize(mf::game::to_json(*task)),
                boost::json::serialize(mf::game::to_json(*retried_task)));
            return mf::core::make_ok_response(
                mf::game::to_json(*retried_task),
                request.version(),
                boost::beast::http::status::created);
        });
}
