#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>

// 文件职责：
// 1. 承接玩家拾取接口和后台生成/查询掉落接口。
// 2. 玩家接口只允许按 drop_id 拾取自己的掉落。
// 3. 后台接口用于联调和运营排查，真正战斗服后续也应走同一套掉落语义。
//
// 文件调用流程：
// 1. main.cpp 构造 LootService。
// 2. main.cpp 调用 register_loot_routes(...)。
// 3. 玩家请求先通过 session 鉴权，再进入 LootService::pickup。
// 4. 后台请求先通过 require_admin_permission 校验 admin.loot_spawn。

void register_loot_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::LootService>& loot_service) {
    app.route(
        mf::http::beast_http::verb::post,
        "/loot/pickup",
        [loot_service, token_service](const mf::http::Request& request) {
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
            const auto drop_id = read_string_field(object, "drop_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("loot.drop_id", drop_id);
            const auto result = loot_service->pickup(session->player_id, drop_id);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/loot/spawn",
        [repositories, token_service, loot_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.loot_spawn",
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
            std::string reward_error;
            const auto rewards = read_rewards_array(object, reward_error);
            if (!rewards) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_loot_rewards",
                        .message = reward_error.empty() ? "掉落奖励配置不合法" : reward_error,
                    },
                    request.version());
            }
            const auto player_id = read_string_field(object, "player_id");
            const auto ttl_seconds = std::max<std::int64_t>(0, read_optional_i64_field(object, "ttl_seconds", 3600));
            const auto result = loot_service->spawn_for_player(
                player_id,
                read_optional_string_field(object, "scene_id"),
                *rewards,
                std::chrono::seconds{ttl_seconds});
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "loot_spawn",
                "player",
                player_id,
                boost::json::serialize(object),
                !result.error,
                result.error ? result.error->message : std::string{});
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/loot/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.loot_spawn",
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
            const auto player_id = read_optional_string_field(object, "player_id");
            const auto include_picked = read_optional_bool_field(object, "include_picked", true);
            const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(
                1,
                std::min<std::int64_t>(100, read_optional_i64_field(object, "limit", 20))));
            std::optional<mf::game::LootDropCursor> drop_cursor;
            const auto last_drop_id = read_optional_string_field(object, "last_drop_id");
            if (!last_drop_id.empty()) {
                drop_cursor = mf::game::LootDropCursor{
                    .last_spawned_at = read_optional_i64_field(object, "last_spawned_at", 0),
                    .last_drop_id = last_drop_id,
                };
            }
            const auto page = repositories.loot->list_drops(
                player_id,
                include_picked,
                std::chrono::system_clock::now(),
                limit,
                drop_cursor);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"items", mf::game::to_json_array(page.items)},
                    {"limit", static_cast<std::int64_t>(limit)},
                    {"has_more", page.has_more},
                    {"next_spawned_at", page.next_spawned_at},
                    {"next_drop_id", page.next_drop_id},
                },
                request.version());
        });
}
