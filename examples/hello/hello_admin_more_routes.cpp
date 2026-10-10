#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"

#include <boost/json/value_to.hpp>

using mf::http::query_param;
using mf::http::read_query_bool;
using mf::http::read_query_size;

// 本文件职责：
// 1. 承接后台入口、管理员用户、审计相关和 GM 聊天控制路由。
// 2. 把后台“身份入口 + 管理控制”集中到一个模块，避免和查询、任务模块混在一起。
// 3. 让管理员登录、权限校验、审计写入和 GM 公告能力形成清晰边界。
//
// 文件调用流程：
// 1. main.cpp 构造 repositories / token_service / account_service / chat_service。
// 2. main.cpp 调用 register_admin_entry_routes / register_admin_user_routes / register_admin_audit_routes / register_admin_chat_routes。
// 3. 这些函数把后台管理和 GM 控制路由挂到 Application。
// 4. 请求进入后先通过 require_admin_permission(...) 或 bootstrap token 校验。
// 5. 再进入管理员仓储、账号服务、审计仓储或聊天服务，最后返回统一 JSON。

// 注册管理员入口路由。
// 调用流程：
// 1. 后台登录先复用普通账号登录，拿到 account/player/session。
// 2. 再检查该账号是否映射为激活中的管理员。
// 3. bootstrap/me/dashboard 则分别负责初始化管理员、返回当前管理员信息、返回后台首页提示。
void register_admin_entry_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const std::string& bootstrap_token) {
    app.route(
        mf::http::beast_http::verb::post,
        "/admin/login",
        [account_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            mf::core::add_current_span_attribute("auth.login_name", read_string_field(object, "login_name"));
            const auto result = account_service->login({
                .login_name = read_string_field(object, "login_name"),
                .password = read_string_field(object, "password"),
                .client_ip = client_ip_from_request(request),
                .user_agent = user_agent_from_request(request),
                .captcha_id = read_optional_string_field(object, "captcha_id"),
                .captcha_answer = read_optional_string_field(object, "captcha_answer"),
                .single_session = read_optional_bool_field(object, "single_session", true),
            });
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }

            const auto admin = repositories.admins->find_by_account_id(result.value->account.account_id);
            if (!admin || !admin->active) {
                token_service->revoke_session_id(result.value->session.session_id);
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::forbidden,
                        .code = "admin_required",
                        .message = "当前账号不是有效的管理员账号",
                    },
                    request.version());
            }
            mf::core::add_current_span_attribute("game.account_id", result.value->account.account_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);

            return mf::core::make_ok_response(
                {
                    {"account", mf::game::to_json(result.value->account)},
                    {"player", mf::game::to_json(result.value->player)},
                    {"session", mf::game::to_json(result.value->session)},
                    {"admin", mf::game::to_json(*admin)},
                    {"dashboard_hint", boost::json::object{{"home", "/admin/dashboard"}, {"menu", "/admin/me"}}},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/bootstrap",
        [repositories, bootstrap_token](const mf::http::Request& request) {
            mf::core::ApiError error;
            if (!require_bootstrap_token(request, bootstrap_token, error)) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto account_id = read_string_field(object, "account_id");
            mf::core::add_current_span_attribute("game.account_id", account_id);
            mf::core::add_current_span_attribute(
                "admin.operator_id",
                read_optional_string_field(object, "operator_name").empty()
                    ? std::string{"bootstrap_admin"}
                    : read_optional_string_field(object, "operator_name"));
            const auto admin = repositories.admins->upsert_admin(
                account_id,
                read_optional_string_field(object, "operator_name").empty()
                    ? std::string{"bootstrap_admin"}
                    : read_optional_string_field(object, "operator_name"),
                read_optional_bool_field(object, "active", true),
                read_string_array_field(object, "permissions"));
            append_admin_audit(
                repositories.audit_logs,
                request,
                "bootstrap",
                "admin_bootstrap",
                "account",
                account_id,
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(admin), request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/me",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.audit_read",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            mf::core::add_current_span_attribute("game.account_id", session_storage.account_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);

            return mf::core::make_ok_response(
                {
                    {"admin", mf::game::to_json(*admin)},
                    {"session", mf::game::to_json(session_storage)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/dashboard",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.audit_read",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            mf::core::add_current_span_attribute("game.account_id", session_storage.account_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);

            return mf::core::make_ok_response(
                {
                    {"admin", mf::game::to_json(*admin)},
                    {"home", "admin_dashboard"},
                    {"message", "admin dashboard is ready"},
                },
                request.version());
        });
}

// 注册管理员账号与风控路由。
// 调用流程：
// 1. 请求先经过管理员权限校验。
// 2. 然后分别落到 admin_account 仓储或 account_service 的风控能力。
// 3. 关键写操作统一追加审计日志，方便后续追查“谁改了什么”。
void register_admin_user_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service) {
    app.route(
        mf::http::beast_http::verb::post,
        "/admin/users/upsert",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.user_manage",
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
            const auto account_id = read_string_field(object, "account_id");
            mf::core::add_current_span_attribute("game.account_id", account_id);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute(
                "admin.permission_count",
                std::to_string(read_string_array_field(object, "permissions").size()));
            const auto before = repositories.admins->find_by_account_id(account_id);
            const auto saved = repositories.admins->upsert_admin(
                account_id,
                read_optional_string_field(object, "operator_name").empty()
                    ? std::string{"admin_operator"}
                    : read_optional_string_field(object, "operator_name"),
                read_optional_bool_field(object, "active", true),
                read_string_array_field(object, "permissions"));
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "admin_user_upsert",
                "account",
                account_id,
                boost::json::serialize(object),
                true,
                {},
                before ? boost::json::serialize(mf::game::to_json(*before)) : std::string{},
                boost::json::serialize(mf::game::to_json(saved)));
            return mf::core::make_ok_response(mf::game::to_json(saved), request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/users",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.user_manage",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto limit = read_query_size(request.target(), "limit", 50);
            const auto offset = read_query_size(request.target(), "offset", 0);
            const auto operator_name_filter = query_param(request.target(), "operator_name");
            const auto active_filter_value = read_query_bool(request.target(), "active");
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (operator_name_filter) {
                mf::core::add_current_span_attribute("filter.operator_name", *operator_name_filter);
            }
            if (active_filter_value.has_value()) {
                mf::core::add_current_span_attribute("filter.active", *active_filter_value ? "true" : "false");
            }
            const auto total = repositories.admins->count_filtered(operator_name_filter, active_filter_value);
            const auto items =
                repositories.admins->list_filtered(limit, offset, operator_name_filter, active_filter_value);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/users/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.user_manage",
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
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 50));
            const auto offset = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "offset", 0));
            const auto operator_name_filter =
                body->as_object().if_contains("operator_name")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "operator_name")}
                    : std::nullopt;
            const auto active_filter =
                body->as_object().if_contains("active")
                    ? std::optional<bool>{read_optional_bool_field(body->as_object(), "active", false)}
                    : std::nullopt;
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (operator_name_filter) {
                mf::core::add_current_span_attribute("filter.operator_name", *operator_name_filter);
            }
            if (active_filter.has_value()) {
                mf::core::add_current_span_attribute("filter.active", *active_filter ? "true" : "false");
            }
            const auto total = repositories.admins->count_filtered(operator_name_filter, active_filter);
            const auto items = repositories.admins->list_filtered(limit, offset, operator_name_filter, active_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(items)}},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/users/detail",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.user_manage",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto account_id = query_param(request.target(), "account_id");
            if (!account_id) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "account_id_required",
                        .message = "必须提供 account_id 查询参数",
                    },
                    request.version());
            }
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("game.account_id", *account_id);
            const auto detail = repositories.admins->find_by_account_id(*account_id);
            if (!detail) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "admin_not_found",
                        .message = "管理员账号不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*detail), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/users/detail",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.user_manage",
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
            const auto detail =
                repositories.admins->find_by_account_id(read_string_field(body->as_object(), "account_id"));
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute(
                "game.account_id",
                read_string_field(body->as_object(), "account_id"));
            if (!detail) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "admin_not_found",
                        .message = "管理员账号不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*detail), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/admin/account-status",
        [account_service, repositories, token_service](const mf::http::Request& request) {
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

            const auto& object = body->as_object();
            const auto status = parse_account_status(read_string_field(object, "status"));
            if (!status) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_account_status",
                        .message = "账号状态只能是 active、frozen 或 banned",
                    },
                    request.version());
            }

            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("game.account_id", read_string_field(object, "account_id"));
            mf::core::add_current_span_attribute("account.status", read_string_field(object, "status"));
            const auto result = account_service->set_account_status(
                read_string_field(object, "account_id"),
                *status,
                read_optional_string_field(object, "reason"));
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "account_status",
                "account",
                read_string_field(object, "account_id"),
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/admin/account-status/batch",
        [account_service, repositories, token_service](const mf::http::Request& request) {
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
            const auto started_at = std::chrono::steady_clock::now();
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto status = parse_account_status(read_string_field(object, "status"));
            if (!status) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_account_status",
                        .message = "账号状态只能是 active、frozen 或 banned",
                    },
                    request.version());
            }

            const auto player_ids = resolve_target_players(object, repositories.players);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("game.player_count", std::to_string(player_ids.size()));
            mf::core::add_current_span_attribute("account.status", read_string_field(object, "status"));
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
                    read_optional_string_field(object, "reason"));
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

            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "account_status_batch",
                "player_batch",
                std::to_string(player_ids.size()),
                boost::json::serialize(object),
                true,
                {},
                {},
                boost::json::serialize(boost::json::object{
                    {"success_count", static_cast<std::int64_t>(success_count)},
                    {"failure_count", static_cast<std::int64_t>(player_ids.size() - success_count)},
                }),
                started_at);

            return mf::core::make_ok_response(
                boost::json::object{
                    {"player_count", static_cast<std::int64_t>(player_ids.size())},
                    {"success_count", static_cast<std::int64_t>(success_count)},
                    {"failure_count", static_cast<std::int64_t>(player_ids.size() - success_count)},
                    {"items", std::move(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/admin/blacklist",
        [account_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.blacklist",
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
            std::optional<std::chrono::system_clock::time_point> expires_at;
            if (object.if_contains("expires_in_seconds")) {
                expires_at = std::chrono::system_clock::now() +
                    std::chrono::seconds{read_optional_i64_field(object, "expires_in_seconds", 0)};
            }
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("auth.subject_type", read_string_field(object, "subject_type"));
            mf::core::add_current_span_attribute("auth.subject_value", read_string_field(object, "subject_value"));
            const auto result = account_service->add_blacklist(
                read_string_field(object, "subject_type"),
                read_string_field(object, "subject_value"),
                read_optional_string_field(object, "reason"),
                expires_at);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "blacklist_add",
                read_string_field(object, "subject_type"),
                read_string_field(object, "subject_value"),
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/items/upsert",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.item_config",
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
            const auto item_id = read_string_field(object, "item_id");
            const auto name = read_string_field(object, "name");
            if (item_id.empty() || name.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "item_required_fields_missing",
                        .message = "item_id 和 name 不能为空",
                    },
                    request.version());
            }
            const auto max_stack = read_optional_i64_field(object, "max_stack", 1);
            if (max_stack <= 0) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_max_stack",
                        .message = "max_stack 必须大于 0",
                    },
                    request.version());
            }

            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("item.item_id", item_id);
            const auto saved = repositories.item_catalog->upsert_item(mf::game::ItemDefinition{
                .item_id = item_id,
                .name = name,
                .description = read_optional_string_field(object, "description"),
                .icon = read_optional_string_field(object, "icon"),
                .rarity = read_optional_string_field(object, "rarity").empty()
                              ? std::string{"3星"}
                              : read_optional_string_field(object, "rarity"),
                .max_stack = max_stack,
                .item_type = read_optional_string_field(object, "item_type").empty()
                                 ? std::string{"material"}
                                 : read_optional_string_field(object, "item_type"),
                .enabled = read_optional_bool_field(object, "enabled", true),
            });
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "item_upsert",
                "item",
                item_id,
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(saved), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/items/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.item_config",
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
            const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(
                1,
                std::min<std::int64_t>(200, read_optional_i64_field(object, "limit", 50))));
            const auto offset = static_cast<std::size_t>(std::max<std::int64_t>(
                0,
                read_optional_i64_field(object, "offset", 0)));
            const auto enabled_filter = object.if_contains("enabled")
                                            ? std::optional<bool>{read_optional_bool_field(object, "enabled", true)}
                                            : std::nullopt;
            const auto item_id_filter = read_optional_string_field(object, "item_id");
            const auto name_filter = read_optional_string_field(object, "name");
            const auto rarity_filter = read_optional_string_field(object, "rarity");
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("item.query.limit", std::to_string(limit));
            const auto total = repositories.item_catalog->count_items(
                item_id_filter.empty() ? std::nullopt : std::optional<std::string>{item_id_filter},
                name_filter.empty() ? std::nullopt : std::optional<std::string>{name_filter},
                rarity_filter.empty() ? std::nullopt : std::optional<std::string>{rarity_filter},
                enabled_filter);
            const auto items = repositories.item_catalog->list_items(
                limit,
                offset,
                item_id_filter.empty() ? std::nullopt : std::optional<std::string>{item_id_filter},
                name_filter.empty() ? std::nullopt : std::optional<std::string>{name_filter},
                rarity_filter.empty() ? std::nullopt : std::optional<std::string>{rarity_filter},
                enabled_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"limit", static_cast<std::int64_t>(limit)},
                    {"offset", static_cast<std::int64_t>(offset)},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });
}

// 注册后台审计查询路由。
// 调用流程：
// 1. 管理员先通过 admin.audit_read 权限校验。
// 2. 再把分页和筛选条件下推到 audit_logs 仓储。
// 3. 最后统一返回 total + items，方便后台表格直接渲染。
void register_admin_audit_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/admin/audit-logs",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.audit_read",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto limit = read_query_size(request.target(), "limit", 100);
            std::optional<mf::game::AuditLogCursor> cursor;
            const auto last_created_at = query_param(request.target(), "last_created_at");
            const auto last_audit_id = query_param(request.target(), "last_audit_id");
            if (last_audit_id) {
                try {
                    cursor = mf::game::AuditLogCursor{
                        .last_created_at = last_created_at ? std::stoll(*last_created_at) : 0,
                        .last_audit_id = *last_audit_id,
                    };
                } catch (const std::exception&) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_audit_cursor",
                            .message = "last_created_at 必须是合法整数",
                        },
                        request.version());
                }
            }
            const auto action_filter = query_param(request.target(), "action");
            const auto operator_filter = query_param(request.target(), "operator_id");
            const auto success_filter_value = read_query_bool(request.target(), "success");
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (action_filter) {
                mf::core::add_current_span_attribute("filter.action", *action_filter);
            }
            if (operator_filter) {
                mf::core::add_current_span_attribute("filter.operator_id", *operator_filter);
            }
            if (success_filter_value.has_value()) {
                mf::core::add_current_span_attribute("filter.success", *success_filter_value ? "true" : "false");
            }
            const auto total = repositories.audit_logs->count_filtered(
                action_filter,
                operator_filter,
                success_filter_value);
            const auto page = repositories.audit_logs->list_recent_filtered(
                limit,
                cursor,
                action_filter,
                operator_filter,
                success_filter_value);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(page.items)},
                    {"has_more", page.has_more},
                    {"next_created_at", page.next_created_at},
                    {"next_audit_id", page.next_audit_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/audit-logs/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.audit_read",
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
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 100));
            std::optional<mf::game::AuditLogCursor> cursor;
            const auto last_audit_id = read_optional_string_field(body->as_object(), "last_audit_id");
            if (!last_audit_id.empty()) {
                cursor = mf::game::AuditLogCursor{
                    .last_created_at = read_optional_i64_field(body->as_object(), "last_created_at", 0),
                    .last_audit_id = last_audit_id,
                };
            }
            const auto action_filter =
                body->as_object().if_contains("action")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "action")}
                    : std::nullopt;
            const auto operator_filter =
                body->as_object().if_contains("operator_id")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "operator_id")}
                    : std::nullopt;
            const auto success_filter =
                body->as_object().if_contains("success")
                    ? std::optional<bool>{read_optional_bool_field(body->as_object(), "success", false)}
                    : std::nullopt;
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            if (action_filter) {
                mf::core::add_current_span_attribute("filter.action", *action_filter);
            }
            if (operator_filter) {
                mf::core::add_current_span_attribute("filter.operator_id", *operator_filter);
            }
            if (success_filter.has_value()) {
                mf::core::add_current_span_attribute("filter.success", *success_filter ? "true" : "false");
            }
            const auto total = repositories.audit_logs->count_filtered(
                action_filter,
                operator_filter,
                success_filter);
            const auto page = repositories.audit_logs->list_recent_filtered(
                limit,
                cursor,
                action_filter,
                operator_filter,
                success_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(page.items)},
                    {"has_more", page.has_more},
                    {"next_created_at", page.next_created_at},
                    {"next_audit_id", page.next_audit_id}},
                request.version());
        });
}

// 注册后台聊天公告路由。
// 调用流程：
// 1. 管理员带着 admin.chat_announce 权限发请求。
// 2. 请求里的频道对象先做校验。
// 3. 再构造一条 system 消息交给 ChatService 发布，并写审计日志。
void register_admin_chat_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::chat::ChatService>& chat_service) {
    app.route(
        mf::http::beast_http::verb::post,
        "/chat/gm/announce",
        [chat_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.chat_announce",
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

            const auto result = chat_service->publish(mf::game::ChatMessage{
                .channel = *channel,
                .kind = mf::game::ChatMessageKind::system,
                .sender_player_id = "gm_system",
                .sender_nickname = "GM",
                .text = read_string_field(body->as_object(), "text"),
                .sent_at = std::chrono::system_clock::now(),
            });
            if (!result.ok || !result.delivered_message) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = result.code.empty() ? "announce_failed" : result.code,
                        .message = result.message.empty() ? "gm announcement failed" : result.message,
                    },
                    request.version());
            }
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            mf::core::add_current_span_attribute("chat.message_kind", "system");
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "chat_gm_announce",
                "channel",
                channel->channel_id,
                boost::json::serialize(body->as_object()));

            if (repositories.announcements) {
                repositories.announcements->publish(
                    *channel,
                    admin->operator_name,
                    "GM公告",
                    read_string_field(body->as_object(), "text"),
                    std::chrono::system_clock::now(),
                    std::nullopt);
            }

            return mf::core::make_ok_response(mf::game::to_json(*result.delivered_message), request.version());
        });
}
