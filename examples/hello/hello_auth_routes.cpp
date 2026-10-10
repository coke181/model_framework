#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/game/json.hpp"

#include <algorithm>
#include <boost/json/object.hpp>

namespace {

// 登录响应只携带进入大厅所需的玩家身份；资产由客户端进入大厅后单独请求。
boost::json::object login_player_json(const mf::game::PlayerProfile& player) {
    return {
        {"player_id", player.player_id},
        {"account_id", player.account_id},
        {"nickname", player.nickname},
    };
}

// 正式登录只返回账号和会话；玩家资料、资产和背包在进入大厅后按需读取。
boost::json::object login_identity_json(const mf::auth::AuthBundle& bundle) {
    return {
        {"account", mf::game::to_json(bundle.account)},
        {"session", mf::game::to_json(bundle.session)},
    };
}

}  // namespace

// 本文件职责：
// 1. 承接账号、登录、密码、会话相关 HTTP 路由。
// 2. 把注册/登录/刷新 token/会话管理从 hello_routes.cpp 中拆出来。
// 3. 保持 main.cpp 只负责装配，不直接承载认证细节。
//
// 文件调用流程：
// 1. main.cpp 构造 account_service / token_service / repositories。
// 2. main.cpp 调用 register_auth_routes(...)。
// 3. 这里把 auth 路由挂到 Application。
// 4. 请求进入后，先解析 JSON 或鉴权，再调用 AccountService / TokenService。
// 5. 最后统一返回账号、玩家、会话或错误结构。

// 注册账号与会话相关路由。
// 调用流程：
// 1. 注册、登录、找回密码一类请求直接进入 AccountService。
// 2. 改密、列会话、踢会话这类接口会先校验 bearer token。
// 3. 刷新 token 和注销则落到 TokenService 完成 session 生命周期管理。
void register_auth_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::auth::AccountService>& account_service,
    mf::core::RuntimeEnvironment runtime_environment) {
    app.route(
        mf::http::beast_http::verb::post,
        "/auth/login",
        [account_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto result = account_service->login({
                .login_name = read_string_field(object, "login_name"),
                .password = read_string_field(object, "password"),
                .client_ip = client_ip_from_request(request),
                .user_agent = user_agent_from_request(request),
                .captcha_id = read_optional_string_field(object, "captcha_id"),
                .captcha_answer = read_optional_string_field(object, "captcha_answer"),
                .single_session = read_optional_bool_field(object, "single_session", false),
            });
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }

            return mf::core::make_ok_response(login_identity_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/register",
        [account_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto result = account_service->register_account({
                .login_name = read_string_field(object, "login_name"),
                .display_name = read_string_field(object, "display_name"),
                .email = read_optional_string_field(object, "email"),
                .phone_number = read_optional_string_field(object, "phone_number"),
                .platform_channel = read_optional_string_field(object, "platform_channel"),
                .register_source = read_optional_string_field(object, "register_source"),
                .password = read_string_field(object, "password"),
                .client_ip = client_ip_from_request(request),
                .user_agent = user_agent_from_request(request),
            });
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }

            return mf::core::make_ok_response(
                {
                    {"account", mf::game::to_json(result.value->account)},
                    {"player", mf::game::to_json(result.value->player)},
                    {"session", mf::game::to_json(result.value->session)},
                },
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/quick-login",
        [account_service, runtime_environment](const mf::http::Request& request) {
            if (mf::core::is_production(runtime_environment)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::forbidden,
                        .code = "quick_login_disabled_in_production",
                        .message = "正式环境已禁用快速登录，请使用正式账号登录",
                    },
                    request.version());
            }
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto login_name = read_string_field(object, "login_name");
            auto display_name = read_optional_string_field(object, "display_name");
            if (display_name.empty()) {
                // quick-login 主要服务开发联调，前端常常只传 login_name。
                // 这里回退成 login_name，避免因为缺少 display_name 直接抛异常变成 500。
                display_name = login_name;
            }
            const auto result = account_service->quick_login(
                login_name,
                std::move(display_name),
                client_ip_from_request(request),
                user_agent_from_request(request));
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }

            return mf::core::make_ok_response(login_identity_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/captcha",
        [account_service](const mf::http::Request& request) {
            const auto challenge = account_service->issue_captcha();
            return mf::core::make_ok_response(mf::game::to_json(challenge), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/change-password",
        [account_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto result = account_service->change_password({
                .account_id = session->account_id,
                .current_password = read_string_field(object, "current_password"),
                .new_password = read_string_field(object, "new_password"),
            });
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }

            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/forgot-password",
        [account_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto result = account_service->forgot_password({
                .login_name = read_string_field(object, "login_name"),
                .email = read_optional_string_field(object, "email"),
            });
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/reset-password",
        [account_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto& object = body->as_object();
            const auto result = account_service->reset_password({
                .reset_token = read_string_field(object, "reset_token"),
                .new_password = read_string_field(object, "new_password"),
            });
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route_async(
        mf::http::beast_http::verb::post,
        "/auth/refresh",
        [token_service](const mf::http::Request& request) -> boost::asio::awaitable<mf::http::Response> {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                co_return mf::core::make_error_response(error, request.version());
            }

            // TokenService -> SessionRepository -> Boost.Redis 全链路都使用 co_await，
            // 等待 Redis 时不会占住 HTTP I/O 线程或同步业务线程。
            const auto refreshed = co_await token_service->refresh_async(
                read_string_field(body->as_object(), "refresh_token"));
            if (!refreshed) {
                co_return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::unauthorized,
                        .code = "refresh_token_invalid",
                        .message = "刷新令牌无效或已过期",
                    },
                    request.version());
            }
            co_return mf::core::make_ok_response(mf::game::to_json(*refreshed), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/logout",
        [token_service](const mf::http::Request& request) {
            const auto authorization = request[boost::beast::http::field::authorization];
            const auto revoked = token_service->revoke_bearer(std::string{authorization});
            if (!revoked) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::unauthorized,
                        .code = "unauthorized",
                        .message = "缺少 Bearer Token，或 Token 无效",
                    },
                    request.version());
            }
            return mf::core::make_ok_response({{"revoked", true}}, request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/auth/sessions",
        [token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            boost::json::array items;
            for (const auto& item : token_service->list_sessions(session->player_id)) {
                items.push_back(mf::game::to_json(item));
            }
            return mf::core::make_ok_response(items, request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/auth/sessions/revoke",
        [token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto session_id = read_string_field(body->as_object(), "session_id");
            const auto my_sessions = token_service->list_sessions(session->player_id);
            const auto own_session = std::find_if(
                my_sessions.begin(),
                my_sessions.end(),
                [&](const mf::game::Session& item) { return item.session_id == session_id; });
            if (own_session == my_sessions.end()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::forbidden,
                        .code = "session_not_owned",
                        .message = "目标会话不属于当前玩家",
                    },
                    request.version());
            }
            token_service->revoke_session_id(session_id);
            return mf::core::make_ok_response({{"revoked_session_id", session_id}}, request.version());
        });
}
