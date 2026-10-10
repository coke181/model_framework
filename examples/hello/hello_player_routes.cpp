#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/chat/channels.hpp"
#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"
#include "mf/http/query_params.hpp"

#include <algorithm>
#include <exception>
#include <limits>

// 本文件职责：
// 1. 承接玩家侧公开业务路由，避免 hello_routes.cpp 继续膨胀。
// 2. 覆盖玩家资料、邮件、好友、聊天四个模块。
// 3. main.cpp 完成服务装配后，会依次调用这里的 register_*_routes 进入各模块注册。
//
// 文件调用流程：
// 1. main.cpp 构造 repositories / services / token_service / chat_service / chat_state_service。
// 2. main.cpp 调用 register_player_routes / register_mail_routes / register_friend_routes / register_chat_routes。
// 3. 这些函数把 HTTP 路由挂到 Application。
// 4. 请求到达后，先通过 require_session 或 require_admin_permission 做鉴权。
// 5. 再进入仓储或服务层，最后通过 make_ok_response / make_error_response 返回统一 JSON。

// 注册玩家资料路由。
// 调用流程：
// 1. `/players/me` 先校验会话，再按 session.player_id 读取资料。
// 2. `/players` 作为示例观察接口，直接返回当前全部玩家。
void register_player_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/players/me",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("game.account_id", session->account_id);
            const auto profile = repositories.players->find_by_player_id(session->player_id);
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
        "/players/me/inventory",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto limit = mf::http::read_query_size(request.target(), "limit", 100);
            const auto cursor = mf::http::query_param(request.target(), "cursor");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);

            const auto page = repositories.players->list_inventory_items(session->player_id, limit, cursor);
            return mf::core::make_ok_response(
                {
                    {"items", mf::game::to_json_array(page.items)},
                    {"limit", static_cast<std::int64_t>(page.items.size())},
                    {"has_more", page.has_more},
                    {"next_cursor", page.next_cursor},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/players",
        [repositories](const mf::http::Request& request) {
            return mf::core::make_ok_response(
                mf::game::to_json_array(repositories.players->list_all()),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/players/me/inventory/item",
        [repositories, token_service](const mf::http::Request& request) {
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
            const auto item_id = read_string_field(object, "item_id");
            if (item_id.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "item_id_required",
                        .message = "item_id 不能为空",
                    },
                    request.version());
            }
            if (!object.if_contains("quantity")) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "quantity_required",
                        .message = "必须提供 quantity，0 表示移除该道具",
                    },
                    request.version());
            }

            const auto quantity = read_optional_i64_field(object, "quantity", 0);
            if (quantity < 0) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_quantity",
                        .message = "quantity 不能小于 0",
                    },
                    request.version());
            }

            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("inventory.item_id", item_id);
            mf::core::add_current_span_attribute("inventory.quantity", std::to_string(quantity));
            const auto profile =
                repositories.players->set_inventory_item_quantity(session->player_id, item_id, quantity);
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
                    {"player", mf::game::to_json(*profile)},
                    {"inventory", mf::game::to_json_array(profile->inventory)},
                },
                request.version());
        });
}

// 注册邮件路由。
// 调用流程：
// 1. 玩家先通过 session 鉴权。
// 2. 然后调用 MailService 做收件箱读取、已读、领取、删除。
// 3. 领取附件时会一并返回邮件、奖励流水和玩家最新状态。
void register_mail_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::game::MailService>& mail_service,
    const std::shared_ptr<mf::auth::TokenService>& token_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/mail",
        [mail_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto include_deleted =
                request.target().find("include_deleted=true") != boost::beast::string_view::npos;
            std::optional<mf::game::MailPageCursor> mail_cursor;
            const auto last_sent_at_param = mf::http::query_param(request.target(), "last_sent_at");
            const auto last_mail_param = mf::http::query_param(request.target(), "last_mail_id");
            if (last_sent_at_param || last_mail_param) {
                if (!last_sent_at_param || !last_mail_param || last_mail_param->empty()) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_mail_cursor",
                            .message = "邮件分页游标需要同时提供 last_sent_at 和 last_mail_id",
                        },
                        request.version());
                }
                try {
                    mail_cursor = mf::game::MailPageCursor{
                        .last_sent_at = std::stoll(*last_sent_at_param),
                        .last_mail_id = *last_mail_param,
                    };
                } catch (const std::exception&) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_mail_cursor",
                            .message = "last_sent_at 必须是合法整数",
                        },
                        request.version());
                }
            }
            const auto requested_limit = mf::http::read_query_size(request.target(), "limit", 100);
            const auto limit =
                requested_limit == 0 ? std::numeric_limits<std::size_t>::max() : requested_limit;
            const auto offset = mail_cursor ? 0 : mf::http::read_query_size(request.target(), "offset", 0);
            const auto fetch_limit = limit == std::numeric_limits<std::size_t>::max() ? limit : limit + 1;
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("mail.include_deleted", include_deleted ? "true" : "false");
            mf::core::add_current_span_attribute("mail.offset", std::to_string(offset));
            const auto total = mail_service->count_inbox_filtered(
                session->player_id, include_deleted, std::nullopt, std::nullopt);
            auto items = mail_cursor
                ? mail_service->list_inbox_cursor(
                      session->player_id, include_deleted, fetch_limit, mail_cursor)
                : mail_service->list_inbox_filtered(
                      session->player_id, include_deleted, limit, offset, std::nullopt, std::nullopt);
            const auto has_more = mail_cursor
                ? limit != std::numeric_limits<std::size_t>::max() && items.size() > limit
                : offset < total && items.size() < total - offset;
            if (mail_cursor && limit != std::numeric_limits<std::size_t>::max() && items.size() > limit) {
                items.resize(limit);
            }
            return mf::core::make_ok_response(
                {
                    {"items", mf::game::to_json_array(items)},
                    {"total", static_cast<std::int64_t>(total)},
                    {"limit", static_cast<std::int64_t>(items.size())},
                    {"offset", static_cast<std::int64_t>(offset)},
                    {"has_more", has_more},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/mail/send-player",
        [repositories, mail_service, token_service](const mf::http::Request& request) {
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
            const auto to_player_id = read_string_field(object, "to_player_id");
            const auto title = read_string_field(object, "title");
            const auto content = read_string_field(object, "body");
            if (to_player_id.empty() || title.empty() || content.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "mail_field_required",
                        .message = "to_player_id、title、body 都不能为空",
                    },
                    request.version());
            }
            if (!repositories.players->find_by_player_id(to_player_id)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "target_player_not_found",
                        .message = "收件玩家不存在",
                    },
                    request.version());
            }

            std::optional<std::chrono::system_clock::time_point> expires_at;
            const auto expires_in_seconds = read_optional_i64_field(object, "expires_in_seconds", 0);
            if (expires_in_seconds > 0) {
                expires_at = std::chrono::system_clock::now() + std::chrono::seconds(expires_in_seconds);
            }

            const auto sender_profile = repositories.players->find_by_player_id(session->player_id);
            const auto sender_name = sender_profile && !sender_profile->nickname.empty()
                                         ? sender_profile->nickname
                                         : session->player_id;
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("mail.to_player_id", to_player_id);
            const auto mail = mail_service->send_mail(
                to_player_id,
                sender_name,
                title,
                content,
                {},
                expires_at);

            return mf::core::make_ok_response(mf::game::to_json(mail), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/mail/read",
        [mail_service, token_service](const mf::http::Request& request) {
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

            const auto mail_id = read_string_field(body->as_object(), "mail_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("mail.mail_id", mail_id);
            const auto result = mail_service->mark_read(session->player_id, mail_id);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/mail/claim",
        [mail_service, token_service](const mf::http::Request& request) {
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

            const auto mail_id = read_string_field(body->as_object(), "mail_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("mail.mail_id", mail_id);
            const auto result = mail_service->claim_mail(session->player_id, mail_id);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            mf::core::add_current_span_attribute("reward.grant_id", result.value->grant.grant_id);

            return mf::core::make_ok_response(
                {
                    {"mail", mf::game::to_json(result.value->mail)},
                    {"grant", mf::game::to_json(result.value->grant)},
                    {"player", mf::game::to_json(result.value->player)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/mail/delete",
        [mail_service, token_service](const mf::http::Request& request) {
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

            const auto mail_id = read_string_field(body->as_object(), "mail_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("mail.mail_id", mail_id);
            const auto result = mail_service->delete_mail(session->player_id, mail_id);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });
}

// 注册好友路由。
// 调用流程：
// 1. 玩家先通过 session 鉴权。
// 2. 然后通过 FriendRepository 处理申请、接受、关系查询和聊天黑名单。
// 3. `/friends` 会把 presence / unread / last_message 合并成一份概览返回。
void register_friend_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::chat::ChatStateService>& chat_state_service,
    const std::shared_ptr<mf::auth::TokenService>& token_service) {
    app.route(
        mf::http::beast_http::verb::post,
        "/friends/add",
        [repositories, token_service](const mf::http::Request& request) {
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

            const auto friend_player_id = read_string_field(body->as_object(), "friend_player_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("social.friend_player_id", friend_player_id);
            if (!repositories.players->find_by_player_id(friend_player_id)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "friend_not_found",
                        .message = "目标玩家不存在",
                    },
                    request.version());
            }

            const auto added = repositories.friends->add_friendship(session->player_id, friend_player_id);
            if (!added) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::conflict,
                        .code = "friendship_conflict",
                        .message = "好友关系已存在，或当前操作不合法",
                    },
                    request.version());
            }

            return mf::core::make_ok_response(
                {
                    {"player_id", session->player_id},
                    {"friend_player_id", friend_player_id},
                },
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/friends/request",
        [repositories, token_service](const mf::http::Request& request) {
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

            const auto target_player_id = read_string_field(body->as_object(), "target_player_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("social.target_player_id", target_player_id);
            if (!repositories.players->find_by_player_id(target_player_id)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "friend_not_found",
                        .message = "目标玩家不存在",
                    },
                    request.version());
            }

            const auto sent = repositories.friends->send_friend_request(session->player_id, target_player_id);
            if (!sent) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::conflict,
                        .code = "friend_request_conflict",
                        .message = "好友申请已存在，或双方已经是好友",
                    },
                    request.version());
            }

            return mf::core::make_ok_response(
                {
                    {"requester_player_id", session->player_id},
                    {"target_player_id", target_player_id},
                },
                request.version(),
                boost::beast::http::status::created);
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/friends/requests",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }
            mf::core::add_current_span_attribute("game.player_id", session->player_id);

            return mf::core::make_ok_response(
                {
                    {"incoming", mf::game::to_json_array(repositories.friends->list_incoming_requests(session->player_id))},
                    {"outgoing", mf::game::to_json_array(repositories.friends->list_outgoing_requests(session->player_id))},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/friends/respond",
        [repositories, token_service](const mf::http::Request& request) {
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

            const auto requester_player_id = read_string_field(body->as_object(), "requester_player_id");
            const auto accept = read_optional_bool_field(body->as_object(), "accept", true);
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("social.requester_player_id", requester_player_id);
            mf::core::add_current_span_attribute("social.accepted", accept ? "true" : "false");
            const auto updated =
                repositories.friends->respond_friend_request(requester_player_id, session->player_id, accept);
            if (!updated) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "friend_request_not_found",
                        .message = "好友申请不存在",
                    },
                    request.version());
            }

            return mf::core::make_ok_response(
                {
                    {"requester_player_id", requester_player_id},
                    {"target_player_id", session->player_id},
                    {"accepted", accept},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/friends",
        [repositories, chat_service, chat_state_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            std::optional<mf::game::FriendPageCursor> friend_cursor;
            const auto last_sent_at_param = mf::http::query_param(request.target(), "last_sent_at");
            const auto last_friend_param = mf::http::query_param(request.target(), "last_friend_player_id");
            if (last_sent_at_param || last_friend_param) {
                if (!last_sent_at_param || !last_friend_param || last_friend_param->empty()) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_friend_cursor",
                            .message = "好友分页游标需要同时提供 last_sent_at 和 last_friend_player_id",
                        },
                        request.version());
                }
                try {
                    friend_cursor = mf::game::FriendPageCursor{
                        .last_sent_at = std::stoll(*last_sent_at_param),
                        .last_friend_player_id = *last_friend_param,
                    };
                } catch (const std::exception&) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_friend_cursor",
                            .message = "last_sent_at 必须是合法整数",
                        },
                        request.version());
                }
            }

            const auto requested_limit = mf::http::read_query_size(request.target(), "limit", 100);
            const auto limit =
                requested_limit == 0 ? std::numeric_limits<std::size_t>::max() : requested_limit;
            const auto offset = friend_cursor ? 0 : mf::http::read_query_size(request.target(), "offset", 0);
            const auto fetch_limit = limit == std::numeric_limits<std::size_t>::max() ? limit : limit + 1;
            auto friends = repositories.friends->list_friends_by_last_message(
                session->player_id,
                fetch_limit,
                offset,
                friend_cursor);
            if (limit != std::numeric_limits<std::size_t>::max() && friends.size() > limit) {
                friends.resize(limit);
            }
            mf::core::add_current_span_attribute("social.friend_page_size", std::to_string(friends.size()));
            mf::core::add_current_span_attribute("social.friend_offset", std::to_string(offset));

            try {
                return mf::core::make_ok_response(
                    build_friend_overview_array(
                        session->player_id,
                        friends,
                        repositories.players,
                        chat_service,
                        chat_state_service,
                        friends.size(),
                        0),
                    request.version());
            } catch (const std::exception& ex) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::service_unavailable,
                        .code = "friend_state_unavailable",
                        .message = "好友在线状态暂时不可用，请稍后重试",
                        .details = {{"reason", ex.what()}},
                    },
                    request.version());
            }
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/friends/presence",
        [repositories, chat_state_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            std::optional<mf::game::FriendPageCursor> friend_cursor;
            const auto last_sent_at_param = mf::http::query_param(request.target(), "last_sent_at");
            const auto last_friend_param = mf::http::query_param(request.target(), "last_friend_player_id");
            if (last_sent_at_param || last_friend_param) {
                if (!last_sent_at_param || !last_friend_param || last_friend_param->empty()) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_friend_cursor",
                            .message = "好友在场分页游标需要同时提供 last_sent_at 和 last_friend_player_id",
                        },
                        request.version());
                }
                try {
                    friend_cursor = mf::game::FriendPageCursor{
                        .last_sent_at = std::stoll(*last_sent_at_param),
                        .last_friend_player_id = *last_friend_param,
                    };
                } catch (const std::exception&) {
                    return mf::core::make_error_response(
                        {
                            .status = boost::beast::http::status::bad_request,
                            .code = "invalid_friend_cursor",
                            .message = "last_sent_at 必须是合法整数",
                        },
                        request.version());
                }
            }

            const auto requested_limit = mf::http::read_query_size(request.target(), "limit", 100);
            const auto limit = std::clamp<std::size_t>(requested_limit, 1, 1000);
            const auto fetch_limit = limit + 1;
            auto friends = repositories.friends->list_friends_by_last_message(
                session->player_id,
                fetch_limit,
                0,
                friend_cursor);
            const auto has_more = friends.size() > limit;
            std::int64_t next_sent_at = 0;
            std::string next_friend_player_id;
            if (has_more) {
                const auto& last_link = friends[limit - 1];
                next_sent_at = last_link.last_sent_at;
                next_friend_player_id = last_link.friend_player_id;
                friends.resize(limit);
            }
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("social.friend_page_size", std::to_string(friends.size()));
            std::vector<std::string> friend_ids;
            friend_ids.reserve(friends.size());
            for (const auto& friend_link : friends) {
                friend_ids.push_back(friend_link.friend_player_id);
            }

            try {
                const auto presence = chat_state_service->list_friend_presence(friend_ids);
                return mf::core::make_ok_response(
                    {
                        {"items", mf::game::to_json_array(presence)},
                        {"has_more", has_more},
                        {"next_sent_at", next_sent_at},
                        {"next_friend_player_id", next_friend_player_id},
                    },
                    request.version());
            } catch (const std::exception& ex) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::service_unavailable,
                        .code = "friend_presence_unavailable",
                        .message = "好友在线状态暂时不可用，请稍后重试",
                        .details = {{"reason", ex.what()}},
                    },
                    request.version());
            }
        });
}

// 注册聊天路由。
// 调用流程：
// 1. 玩家或管理员先完成 session / permission 校验。
// 2. 然后通过 ChatService / ChatStateService / FriendRepository 完成历史、未读、撤回、禁言等逻辑。
// 3. 这组接口既服务聊天 UI，也服务后台的实时管控能力。
void register_chat_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::chat::ChatService>& chat_service,
    const std::shared_ptr<mf::chat::ChatStateService>& chat_state_service,
    const std::shared_ptr<mf::auth::TokenService>& token_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/chat/history/lobby",
        [chat_service](const mf::http::Request& request) {
            const auto lobby = mf::chat::make_lobby_channel();
            mf::core::add_current_span_attribute("chat.channel_id", lobby.channel_id);
            const auto limit = mf::http::read_query_size(request.target(), "limit", 20);
            const auto cursor = mf::http::query_param(request.target(), "cursor").value_or(std::string{});
            const auto page = chat_service->history_page(lobby.channel_id, cursor, limit);
            return mf::core::make_ok_response(
                {
                    {"messages", mf::game::to_json_array(page.messages)},
                    {"has_more", page.has_more},
                    {"next_cursor", page.next_cursor},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/chat/unread",
        [chat_state_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }
            mf::core::add_current_span_attribute("game.player_id", session->player_id);

            return mf::core::make_ok_response(
                mf::game::to_json_array(chat_state_service->list_unread_counts(session->player_id)),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/block",
        [repositories, token_service](const mf::http::Request& request) {
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

            const auto target_player_id = read_string_field(body->as_object(), "target_player_id");
            const auto blocked = read_optional_bool_field(body->as_object(), "blocked", true);
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("social.target_player_id", target_player_id);
            mf::core::add_current_span_attribute("chat.blocked", blocked ? "true" : "false");
            repositories.friends->set_chat_block(session->player_id, target_player_id, blocked);
            return mf::core::make_ok_response(
                {
                    {"player_id", session->player_id},
                    {"target_player_id", target_player_id},
                    {"blocked", blocked},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/chat/blocks",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }
            mf::core::add_current_span_attribute("game.player_id", session->player_id);

            return mf::core::make_ok_response(
                mf::game::to_json_array(repositories.friends->list_chat_blocks(session->player_id)),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/mute",
        [chat_service, repositories, token_service](const mf::http::Request& request) {
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

            const auto duration_seconds = read_optional_i64_field(body->as_object(), "duration_seconds", 3600);
            const auto muted_player_id = read_string_field(body->as_object(), "muted_player_id");
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            mf::core::add_current_span_attribute("chat.muted_player_id", muted_player_id);
            const auto mute = chat_service->mute_player(
                channel->channel_id,
                muted_player_id,
                read_optional_string_field(body->as_object(), "muted_by_player_id").empty()
                    ? admin->operator_name
                    : read_optional_string_field(body->as_object(), "muted_by_player_id"),
                read_optional_string_field(body->as_object(), "reason"),
                std::chrono::system_clock::now() + std::chrono::seconds{duration_seconds});
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "chat_mute",
                "channel",
                channel->channel_id,
                boost::json::serialize(body->as_object()));
            return mf::core::make_ok_response(mf::game::to_json(mute), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/mute/clear",
        [chat_service, repositories, token_service](const mf::http::Request& request) {
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
            const auto started_at = std::chrono::steady_clock::now();
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

            const auto muted_player_id = read_string_field(body->as_object(), "muted_player_id");
            if (muted_player_id.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "muted_player_id_required",
                        .message = "要取消禁言的 muted_player_id 不能为空",
                    },
                    request.version());
            }

            // 清除指定频道里的禁言记录；如果玩家本来没有被禁言，仓储层按幂等删除处理。
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            mf::core::add_current_span_attribute("chat.muted_player_id", muted_player_id);
            chat_service->clear_mute(channel->channel_id, muted_player_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "chat_mute_clear",
                "channel",
                channel->channel_id,
                boost::json::serialize(body->as_object()),
                true,
                {},
                {},
                boost::json::serialize(boost::json::object{
                    {"muted_player_id", muted_player_id},
                    {"cleared", true},
                }),
                started_at);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"channel_id", channel->channel_id},
                    {"muted_player_id", muted_player_id},
                    {"cleared", true},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/mute/batch",
        [chat_service, repositories, token_service](const mf::http::Request& request) {
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
            const auto started_at = std::chrono::steady_clock::now();
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

            const auto duration_seconds = read_optional_i64_field(body->as_object(), "duration_seconds", 3600);
            const auto player_ids = resolve_target_players(body->as_object(), repositories.players);
            mf::core::add_current_span_attribute("admin.operator_id", admin->operator_name);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            mf::core::add_current_span_attribute("game.player_count", std::to_string(player_ids.size()));
            boost::json::array items;
            std::size_t success_count = 0;
            for (const auto& player_id : player_ids) {
                const auto mute = chat_service->mute_player(
                    channel->channel_id,
                    player_id,
                    read_optional_string_field(body->as_object(), "muted_by_player_id").empty()
                        ? admin->operator_name
                        : read_optional_string_field(body->as_object(), "muted_by_player_id"),
                    read_optional_string_field(body->as_object(), "reason"),
                    std::chrono::system_clock::now() + std::chrono::seconds{duration_seconds});
                ++success_count;
                items.push_back(boost::json::object{
                    {"player_id", player_id},
                    {"success", true},
                    {"mute", mf::game::to_json(mute)},
                });
            }

            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "chat_mute_batch",
                "channel",
                channel->channel_id,
                boost::json::serialize(body->as_object()),
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
                    {"channel_id", channel->channel_id},
                    {"player_count", static_cast<std::int64_t>(player_ids.size())},
                    {"success_count", static_cast<std::int64_t>(success_count)},
                    {"failure_count", static_cast<std::int64_t>(player_ids.size() - success_count)},
                    {"items", std::move(items)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/recall",
        [chat_service, token_service](const mf::http::Request& request) {
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

            const auto recalled = chat_service->recall(
                channel->channel_id,
                read_string_field(body->as_object(), "message_id"),
                session->player_id);
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            mf::core::add_current_span_attribute("chat.message_id", read_string_field(body->as_object(), "message_id"));
            if (!recalled) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "message_not_found",
                        .message = "消息不存在，或已经被撤回",
                    },
                    request.version());
            }

            return mf::core::make_ok_response(mf::game::to_json(*recalled), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/read",
        [chat_state_service, token_service](const mf::http::Request& request) {
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

            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            chat_state_service->mark_channel_read(session->player_id, channel->channel_id);
            return mf::core::make_ok_response(
                {
                    {"player_id", session->player_id},
                    {"channel_id", channel->channel_id},
                    {"read", true},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/offline/pull",
        [chat_state_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }

            std::size_t limit = 20;
            if (!request.body().empty()) {
                const auto body = mf::core::parse_json_body(request, error);
                if (!body) {
                    return mf::core::make_error_response(error, request.version());
                }
                if (body->as_object().if_contains("limit")) {
                    limit = static_cast<std::size_t>(read_optional_i64_field(body->as_object(), "limit", 20));
                }
            }
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("chat.limit", std::to_string(limit));

            return mf::core::make_ok_response(
                mf::game::to_json_array(chat_state_service->pull_offline_messages(session->player_id, limit)),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/chat/channels/examples",
        [](const mf::http::Request& request) {
            const auto lobby = mf::chat::make_lobby_channel();
            const auto room = mf::chat::make_room_channel("room_1001");
            const auto friend_channel = mf::chat::make_friend_channel("player_1", "player_2");
            return mf::core::make_ok_response(
                {
                    {"lobby", mf::game::to_json(lobby)},
                    {"room", mf::game::to_json(room)},
                    {"friend_direct", mf::game::to_json(friend_channel)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/presence/player",
        [chat_state_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }

            const auto player_id = read_string_field(body->as_object(), "player_id");
            mf::core::add_current_span_attribute("game.player_id", player_id);
            return mf::core::make_ok_response(
                {
                    {"player_id", player_id},
                    {"online", chat_state_service->is_online(player_id)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/chat/presence/channel",
        [chat_state_service](const mf::http::Request& request) {
            mf::core::ApiError error;
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

            const auto& object = body->as_object();
            const auto limit = static_cast<std::size_t>(read_optional_i64_field(object, "limit", 100));
            const auto cursor = read_optional_string_field(object, "cursor");
            const auto page = chat_state_service->list_channel_members(channel->channel_id, cursor, limit);
            mf::core::add_current_span_attribute("chat.channel_id", channel->channel_id);
            mf::core::add_current_span_attribute("chat.member_count", std::to_string(page.total_count));
            boost::json::array member_array;
            member_array.reserve(page.members.size());
            for (const auto& member : page.members) {
                member_array.push_back(boost::json::value{member});
            }

            return mf::core::make_ok_response(
                {
                    {"channel", mf::game::to_json(*channel)},
                    {"member_count", page.total_count},
                    {"has_more", page.has_more},
                    {"next_cursor", page.next_cursor},
                    {"members", std::move(member_array)},
                },
                request.version());
        });
}
