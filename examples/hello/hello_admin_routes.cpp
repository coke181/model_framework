#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/chat/channels.hpp"
#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"

#include <boost/json/value_to.hpp>

#include <algorithm>
#include <chrono>

using mf::http::query_param;
using mf::http::read_query_bool;
using mf::http::read_query_bool_or;
using mf::http::read_query_size;

// 本文件职责：
// 1. 承接后台查询类路由，覆盖账号、玩家、邮件、奖励、聊天等后台查阅入口。
// 2. 把以“读”为主的后台接口从其他管理模块中分离出来，降低单文件复杂度。
// 3. 统一处理分页、筛选和结果包装，方便后台页面直接消费。
//
// 文件调用流程：
// 1. main.cpp 构造 repositories / account_service / reward_service / mail_service / chat_service。
// 2. main.cpp 调用 register_admin_query_routes(...)。
// 3. 这里把后台查询路由挂到 Application。
// 4. 请求进入后先校验管理员 token 和 admin.query 权限。
// 5. 再把筛选条件下推到服务层或仓储层，最后返回 total + items 或单对象结果。

// 注册后台查询路由。
// 调用流程：
// 1. HTTP 请求进入 Application 路由表。
// 2. 先校验管理员 token 和 admin.query 权限。
// 3. 再把查询条件交给 account/player/mail/reward/chat 等服务或仓储。
// 4. 最后统一返回 `{ total, items }` 或单对象结果，方便后台页面直接消费。
void register_admin_query_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    const std::shared_ptr<mf::game::RewardService>& reward_service,
    const std::shared_ptr<mf::game::MailService>& mail_service,
    const std::shared_ptr<mf::chat::ChatService>& chat_service) {
    auto build_player_admin_rows =
        [](const std::vector<mf::game::PlayerAdminRow>& rows) {
            const auto status_text = [](mf::game::AccountStatus status) {
                switch (status) {
                case mf::game::AccountStatus::active:
                    return "active";
                case mf::game::AccountStatus::frozen:
                    return "frozen";
                case mf::game::AccountStatus::banned:
                    return "banned";
                }
                return "active";
            };
            boost::json::array items;
            items.reserve(rows.size());
            for (const auto& row : rows) {
                items.push_back(boost::json::object{
                    {"player_id", row.player_id},
                    {"account_id", row.account_id},
                    {"nickname", row.nickname},
                    {"gold", row.gold},
                    {"gem", row.gem},
                    {"status", boost::json::string{status_text(row.status)}},
                    {"mute_channel_id", row.mute_channel_id},
                    {"muted_until", row.muted_until ? boost::json::value{std::chrono::duration_cast<std::chrono::seconds>(row.muted_until->time_since_epoch()).count()} : boost::json::value{}},
                    {"last_login_at", row.last_login_at ? boost::json::value{std::chrono::duration_cast<std::chrono::seconds>(row.last_login_at->time_since_epoch()).count()} : boost::json::value{}},
                    {"last_login_ip", row.last_login_ip},
                });
            }
            return items;
        };

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/account",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            std::optional<mf::game::Account> account;
            if (const auto account_id = query_param(request.target(), "account_id")) {
                account = repositories.accounts->find_by_account_id(*account_id);
            } else if (const auto login_name = query_param(request.target(), "login_name")) {
                account = repositories.accounts->find_by_login_name(*login_name);
            }
            if (!account) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "account_not_found",
                        .message = "账号不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*account), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/account",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            std::optional<mf::game::Account> account;
            if (object.if_contains("account_id")) {
                mf::core::add_current_span_attribute("game.account_id", read_string_field(object, "account_id"));
                account = repositories.accounts->find_by_account_id(read_string_field(object, "account_id"));
            } else if (object.if_contains("login_name")) {
                mf::core::add_current_span_attribute("auth.login_name", read_string_field(object, "login_name"));
                account = repositories.accounts->find_by_login_name(read_string_field(object, "login_name"));
            }
            if (!account) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "account_not_found",
                        .message = "账号不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*account), request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/player",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto player_id = query_param(request.target(), "player_id");
            if (!player_id) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "player_id_required",
                        .message = "必须提供 player_id 查询参数",
                    },
                    request.version());
            }

            const auto profile = repositories.players->find_by_player_id(*player_id);
            if (!profile) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "player_not_found",
                        .message = "玩家资料不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*profile), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/player",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            const auto profile =
                [&] {
                    const auto player_id = read_string_field(body->as_object(), "player_id");
                    mf::core::add_current_span_attribute("game.player_id", player_id);
                    return repositories.players->find_by_player_id(player_id);
                }();
            if (!profile) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "player_not_found",
                        .message = "玩家资料不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*profile), request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/players",
        [repositories, token_service, build_player_admin_rows](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto limit = read_query_size(request.target(), "limit", 50);
            std::optional<mf::game::PlayerPageCursor> cursor;
            const auto last_created_at = query_param(request.target(), "last_created_at");
            const auto last_player_id = query_param(request.target(), "last_player_id");
            if (last_player_id) {
                try {
                    cursor = mf::game::PlayerPageCursor{
                        .last_created_at = last_created_at ? std::stoll(*last_created_at) : 0,
                        .last_player_id = *last_player_id,
                    };
                } catch (const std::exception&) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_player_cursor",
                            .message = "last_created_at 必须是合法整数",
                        },
                        request.version());
                }
            }
            const auto player_id_filter = query_param(request.target(), "player_id");
            const auto account_id_filter = query_param(request.target(), "account_id");
            const auto nickname_filter = query_param(request.target(), "nickname");
            const auto total = repositories.players->count_filtered(
                player_id_filter,
                account_id_filter,
                nickname_filter);
            const auto page = repositories.players->list_admin_rows(
                limit,
                cursor,
                player_id_filter,
                account_id_filter,
                nickname_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", build_player_admin_rows(page.rows)},
                    {"has_more", page.has_more},
                    {"next_created_at", page.next_created_at},
                    {"next_player_id", page.next_player_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/players",
        [repositories, token_service, build_player_admin_rows](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            std::optional<mf::game::PlayerPageCursor> cursor;
            const auto last_player_id = read_optional_string_field(body->as_object(), "last_player_id");
            if (!last_player_id.empty()) {
                cursor = mf::game::PlayerPageCursor{
                    .last_created_at = read_optional_i64_field(body->as_object(), "last_created_at", 0),
                    .last_player_id = last_player_id,
                };
            }
            const auto player_id_filter =
                body->as_object().if_contains("player_id")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "player_id")}
                    : std::nullopt;
            const auto account_id_filter =
                body->as_object().if_contains("account_id")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "account_id")}
                    : std::nullopt;
            const auto nickname_filter =
                body->as_object().if_contains("nickname")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "nickname")}
                    : std::nullopt;
            if (player_id_filter) mf::core::add_current_span_attribute("filter.player_id", *player_id_filter);
            if (account_id_filter) mf::core::add_current_span_attribute("filter.account_id", *account_id_filter);
            if (nickname_filter) mf::core::add_current_span_attribute("filter.nickname", *nickname_filter);
            const auto total = repositories.players->count_filtered(
                player_id_filter,
                account_id_filter,
                nickname_filter);
            const auto page = repositories.players->list_admin_rows(
                limit,
                cursor,
                player_id_filter,
                account_id_filter,
                nickname_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", build_player_admin_rows(page.rows)},
                    {"has_more", page.has_more},
                    {"next_created_at", page.next_created_at},
                    {"next_player_id", page.next_player_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/inventory",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto player_id = query_param(request.target(), "player_id");
            if (!player_id) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "player_id_required",
                        .message = "必须提供 player_id 查询参数",
                    },
                    request.version());
            }

            const auto profile = repositories.players->find_by_player_id(*player_id);
            if (!profile) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "player_not_found",
                        .message = "玩家资料不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(
                {
                    {"player_id", profile->player_id},
                    {"inventory", mf::game::to_json_array(profile->inventory)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/inventory",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            const auto profile =
                repositories.players->find_by_player_id(read_string_field(body->as_object(), "player_id"));
            if (!profile) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "player_not_found",
                        .message = "玩家资料不存在",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(
                {
                    {"player_id", profile->player_id},
                    {"inventory", mf::game::to_json_array(profile->inventory)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/mail",
        [mail_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto player_id = query_param(request.target(), "player_id");
            if (!player_id) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "player_id_required",
                        .message = "必须提供 player_id 查询参数",
                    },
                    request.version());
            }
            const auto include_deleted = read_query_bool_or(request.target(), "include_deleted", false);
            const auto claimed_filter = read_query_bool(request.target(), "claimed");
            const auto read_filter = read_query_bool(request.target(), "read");
            const auto limit = read_query_size(request.target(), "limit", 50);
            const auto offset = read_query_size(request.target(), "offset", 0);
            const auto total =
                mail_service->count_inbox_filtered(*player_id, include_deleted, claimed_filter, read_filter);
            const auto items = mail_service->list_inbox_filtered(
                *player_id,
                include_deleted,
                limit,
                offset,
                claimed_filter,
                read_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/mail",
        [mail_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            const auto player_id = read_string_field(body->as_object(), "player_id");
            const auto include_deleted = read_optional_bool_field(body->as_object(), "include_deleted", false);
            const auto claimed_filter =
                body->as_object().if_contains("claimed")
                    ? std::optional<bool>{read_optional_bool_field(body->as_object(), "claimed", false)}
                    : std::nullopt;
            const auto read_filter =
                body->as_object().if_contains("read")
                    ? std::optional<bool>{read_optional_bool_field(body->as_object(), "read", false)}
                    : std::nullopt;
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 50));
            const auto offset = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "offset", 0));
            const auto total =
                mail_service->count_inbox_filtered(player_id, include_deleted, claimed_filter, read_filter);
            const auto items = mail_service->list_inbox_filtered(
                player_id,
                include_deleted,
                limit,
                offset,
                claimed_filter,
                read_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/reward-grants",
        [reward_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto player_id = query_param(request.target(), "player_id");
            if (!player_id) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "player_id_required",
                        .message = "必须提供 player_id 查询参数",
                    },
                    request.version());
            }
            const auto limit = read_query_size(request.target(), "limit", 50);
            const auto offset = read_query_size(request.target(), "offset", 0);
            const auto source_type_filter = query_param(request.target(), "source_type");
            const auto source_id_filter = query_param(request.target(), "source_id");
            const auto total =
                reward_service->count_grants_filtered(*player_id, source_type_filter, source_id_filter);
            const auto items = reward_service->list_grants_filtered(
                *player_id,
                limit,
                offset,
                source_type_filter,
                source_id_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/reward-grants",
        [reward_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            const auto player_id = read_string_field(body->as_object(), "player_id");
            const auto source_type_filter =
                body->as_object().if_contains("source_type")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "source_type")}
                    : std::nullopt;
            const auto source_id_filter =
                body->as_object().if_contains("source_id")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "source_id")}
                    : std::nullopt;
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 50));
            const auto offset = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "offset", 0));
            const auto total =
                reward_service->count_grants_filtered(player_id, source_type_filter, source_id_filter);
            const auto items = reward_service->list_grants_filtered(
                player_id,
                limit,
                offset,
                source_type_filter,
                source_id_filter);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(total)},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/chat-history",
        [chat_service, repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            return mf::core::make_ok_response(
                mf::game::to_json_array(
                    chat_service->history(
                        channel->channel_id,
                        static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 50)))),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/announcements",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            std::optional<mf::game::ChatChannelKind> kind_filter;
            if (const auto* value = body->as_object().if_contains("channel_kind")) {
                kind_filter = mf::chat::parse_channel_kind(boost::json::value_to<std::string>(*value));
            }
            const auto scope_filter =
                body->as_object().if_contains("scope_id")
                    ? std::optional<std::string>{read_string_field(body->as_object(), "scope_id")}
                    : std::nullopt;
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 20));
            const auto items = repositories.announcements
                ? repositories.announcements->list_recent(limit, kind_filter, scope_filter, std::chrono::system_clock::now())
                : std::vector<mf::game::AnnouncementEntry>{};
            return mf::core::make_ok_response(
                boost::json::object{
                    {"total", static_cast<std::int64_t>(items.size())},
                    {"items", mf::game::to_json_array(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/blacklists",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            std::size_t limit = 100;
            if (const auto limit_value = query_param(request.target(), "limit")) {
                limit = static_cast<std::size_t>(std::stoll(*limit_value));
            }
            return mf::core::make_ok_response(
                mf::game::to_json_array(repositories.auth_security->list_blacklist_entries(limit)),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/blacklists",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
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
            return mf::core::make_ok_response(
                mf::game::to_json_array(repositories.auth_security->list_blacklist_entries(
                    static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 100)))),
                request.version());
        });

    // 查询聊天 Outbox 健康状态，供后台页面和告警轮询。
    // 这里只返回数量和积压年龄，不返回聊天正文，避免调试接口泄露玩家消息。
    app.route(
        mf::http::beast_http::verb::get,
        "/admin/query/chat-outbox",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.query",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto health = repositories.chat->outbox_health();
            return mf::core::make_ok_response(
                boost::json::object{
                    {"pending_count", health.pending_count},
                    {"failed_count", health.failed_count},
                    {"oldest_pending_age_seconds", health.oldest_pending_age_seconds},
                    {"healthy", health.failed_count == 0 && health.oldest_pending_age_seconds < 300},
                },
                request.version());
        });

    // 人工恢复达到失败阈值的 Outbox 消息。
    // 这是受保护的运维动作，只允许按最早失败记录批量重新投递，并写入审计日志。
    app.route(
        mf::http::beast_http::verb::post,
        "/admin/query/chat-outbox/retry",
        [repositories, token_service](const mf::http::Request& request) {
            const auto started_at = std::chrono::steady_clock::now();
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.chat_outbox_retry",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto body = mf::core::parse_json_body(request, error);
            if (!body || !body->is_object()) {
                if (!body) {
                    return mf::core::make_error_response(error, request.version());
                }
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_request_body",
                        .message = "请求体必须是 JSON 对象",
                    },
                    request.version());
            }

            const auto requested_limit = read_optional_i64_field(body->as_object(), "limit", 10);
            const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(requested_limit, 1, 100));
            const auto retried = repositories.chat->retry_failed_outbox(limit);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->account_id,
                "chat.outbox.retry_failed",
                "chat_message_outbox",
                "batch",
                boost::json::serialize(boost::json::object{{"limit", limit}, {"retried", retried}}),
                true,
                {},
                {},
                {},
                started_at);

            return mf::core::make_ok_response(
                boost::json::object{
                    {"retried", retried},
                    {"message", "失败的聊天 Outbox 消息已重新加入处理队列"},
                },
                request.version());
        });
}
