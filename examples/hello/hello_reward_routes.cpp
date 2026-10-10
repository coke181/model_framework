#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"

// 本文件职责：
// 1. 承接奖励、奖励模板、批量发奖、邮件投递相关路由。
// 2. 把带审计、带后台任务记录的运营投放接口从 hello_routes.cpp 中拆出来。
// 3. 让系统路由和运营投递路由彻底分层，方便单独 review。
//
// 文件调用流程：
// 1. main.cpp 构造 reward_service / mail_service / repositories / token_service。
// 2. main.cpp 调用 register_reward_routes(...)。
// 3. 这里把奖励与邮件投递路由挂到 Application。
// 4. 请求进入后先做管理员或玩家鉴权，再解析 rewards / player_ids 等参数。
// 5. 最后调用 RewardService / MailService，并补写审计日志和后台任务。

// 注册奖励、奖励模板和后台批量投递路由。
// 调用流程：
// 1. 管理员写接口先做权限校验。
// 2. 奖励内容先解析成统一 RewardEntry 数组。
// 3. 再调用 RewardService / MailService 执行，必要时补一条后台任务记录和审计日志。
void register_reward_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::RewardService>& reward_service,
    const std::shared_ptr<mf::game::MailService>& mail_service,
    const std::shared_ptr<AdminTaskScheduler>& admin_task_scheduler) {
    app.route(
        mf::http::beast_http::verb::post,
        "/rewards/grant",
        [repositories, reward_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.reward_grant",
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

            const auto& object = body->as_object();
            const auto player_id = read_string_field(object, "player_id");
            mf::core::add_current_span_attribute("game.player_id", player_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (!repositories.players->find_by_player_id(player_id)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "player_not_found",
                        .message = "目标玩家不存在",
                    },
                    request.version());
            }

            std::string reward_error_message;
            const auto rewards = read_rewards_array(object, reward_error_message);
            if (!rewards) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_rewards",
                        .message = reward_error_message,
                    },
                    request.version());
            }

            const auto grant_id = object.if_contains("grant_id")
                                      ? read_string_field(object, "grant_id")
                                      : "gm_grant:" + player_id + ":" +
                                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               std::chrono::system_clock::now().time_since_epoch())
                                                               .count());
            const auto result = reward_service->grant_to_player(
                player_id,
                grant_id,
                read_optional_string_field(object, "source_type").empty()
                    ? std::string{"gm"}
                    : read_optional_string_field(object, "source_type"),
                read_optional_string_field(object, "source_id"),
                *rewards);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "reward_grant",
                "player",
                player_id,
                boost::json::serialize(object));

            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/reward-templates",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.reward_template",
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

            const auto& object = body->as_object();
            std::string reward_error_message;
            const auto rewards = read_rewards_array(object, reward_error_message);
            if (!rewards) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_rewards",
                        .message = reward_error_message,
                    },
                    request.version());
            }

            const auto reward_template = repositories.reward_templates->upsert_template(
                read_string_field(object, "template_id"),
                read_string_field(object, "name"),
                *rewards);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "reward_template_upsert",
                "reward_template",
                reward_template.template_id,
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(reward_template), request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/reward-templates",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.reward_template",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            return mf::core::make_ok_response(
                mf::game::to_json_array(repositories.reward_templates->list_all()),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/rewards/grants",
        [reward_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            return mf::core::make_ok_response(
                mf::game::to_json_array(reward_service->list_grants(session->player_id, 50)),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/batch/rewards",
        [repositories, token_service, admin_task_scheduler](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.batch_reward",
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
            const auto& object = body->as_object();
            std::string reward_error_message;
            const auto rewards = read_rewards_array(object, reward_error_message);
            if (!rewards) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_rewards",
                        .message = reward_error_message,
                    },
                    request.version());
            }

            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            const auto task = admin_task_scheduler->schedule_batch_reward(
                object,
                admin->operator_name,
                client_ip_from_request(request),
                user_agent_from_request(request),
                trace_id_from_request(request));
            mf::core::add_current_span_attribute("task.id", task.task_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "batch_reward_task_schedule",
                "player_batch",
                task.target_id,
                boost::json::serialize(object),
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
        "/mail/send",
        [repositories, mail_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.mail_send",
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

            const auto& object = body->as_object();
            const auto title = read_string_field(object, "title");
            const auto content = read_string_field(object, "body");
            if (title.empty() || content.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "mail_content_required",
                        .message = "邮件标题和正文不能为空",
                        .details = {{"fields", boost::json::array{"title", "body"}}},
                    },
                    request.version());
            }

            std::string reward_error_message;
            const auto rewards = read_rewards_array(object, reward_error_message);
            if (!rewards) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_rewards",
                        .message = reward_error_message,
                    },
                    request.version());
            }

            std::optional<std::chrono::system_clock::time_point> expires_at;
            if (object.if_contains("expires_in_seconds")) {
                expires_at = std::chrono::system_clock::now() +
                    std::chrono::seconds{read_optional_i64_field(object, "expires_in_seconds", 0)};
            }

            if (object.if_contains("player_ids")) {
                const auto player_ids = read_string_array_field(object, "player_ids");
                mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
                mf::core::add_current_span_attribute("game.player_count", std::to_string(player_ids.size()));
                append_admin_audit(
                    repositories.audit_logs,
                    request,
                    admin->operator_name,
                    "mail_send_batch",
                    "player_batch",
                    std::to_string(player_ids.size()),
                    boost::json::serialize(object));
                return mf::core::make_ok_response(
                    mf::game::to_json_array(mail_service->send_batch(
                        player_ids,
                        read_optional_string_field(object, "sender_name").empty()
                            ? std::string{"System"}
                            : read_optional_string_field(object, "sender_name"),
                        title,
                        content,
                        *rewards,
                        expires_at)),
                    request.version(),
                    boost::beast::http::status::created);
            }

            const auto player_id = read_string_field(object, "player_id");
            mf::core::add_current_span_attribute("game.player_id", player_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (!repositories.players->find_by_player_id(player_id)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "player_not_found",
                        .message = "目标玩家不存在",
                    },
                    request.version());
            }

            return mf::core::make_ok_response(
                mf::game::to_json([&] {
                    const auto mail = mail_service->send_mail(
                        player_id,
                        read_optional_string_field(object, "sender_name").empty()
                            ? std::string{"System"}
                            : read_optional_string_field(object, "sender_name"),
                        title,
                        content,
                        *rewards,
                        expires_at);
                    append_admin_audit(
                        repositories.audit_logs,
                        request,
                        admin->operator_name,
                        "mail_send",
                        "player",
                        player_id,
                        boost::json::serialize(object));
                    return mail;
                }()),
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/batch/mails",
        [repositories, token_service, admin_task_scheduler](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.batch_mail",
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
            const auto& object = body->as_object();
            const auto title = read_string_field(object, "title");
            const auto content = read_string_field(object, "body");
            if (title.empty() || content.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "mail_content_required",
                        .message = "邮件标题和正文不能为空",
                        .details = {{"fields", boost::json::array{"title", "body"}}},
                    },
                    request.version());
            }

            std::string reward_error_message;
            const auto rewards = read_rewards_array(object, reward_error_message);
            if (!rewards) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_rewards",
                        .message = reward_error_message,
                    },
                    request.version());
            }

            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            const auto task = admin_task_scheduler->schedule_batch_mail(
                object,
                admin->operator_name,
                client_ip_from_request(request),
                user_agent_from_request(request),
                trace_id_from_request(request));
            mf::core::add_current_span_attribute("task.id", task.task_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "batch_mail_task_schedule",
                "player_batch",
                task.target_id,
                boost::json::serialize(object),
                true,
                {},
                {},
                boost::json::serialize(mf::game::to_json(task)));
            return mf::core::make_ok_response(
                mf::game::to_json(task),
                request.version(),
                boost::beast::http::status::created);
        });
}
