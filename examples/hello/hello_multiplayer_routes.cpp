#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <chrono>

// 文件职责：提供权威多人房间的创建、加入、查询、退出和关闭接口。
// 调用流程：游戏 Bearer Token -> MultiplayerRoomService -> 多人房间仓储 -> 自动同步 VoiceService。

namespace {

std::int64_t unix_seconds(
    const std::optional<std::chrono::system_clock::time_point>& value) {
    if (!value) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(value->time_since_epoch()).count();
}

boost::json::object member_json(const mf::multiplayer::MultiplayerRoomMember& member) {
    return {
        {"room_id", member.room_id},
        {"player_id", member.player_id},
        {"display_name", member.display_name},
        {"role", member.role},
        {"joined_at", unix_seconds(member.joined_at)},
    };
}

boost::json::object snapshot_json(const mf::multiplayer::MultiplayerRoomSnapshot& snapshot) {
    boost::json::array members;
    for (const auto& member : snapshot.members) members.push_back(member_json(member));
    return {
        {"room_id", snapshot.room.room_id},
        {"owner_player_id", snapshot.room.owner_player_id},
        {"room_name", snapshot.room.room_name},
        {"max_players", snapshot.room.max_players},
        {"status", snapshot.room.status},
        {"created_at", unix_seconds(snapshot.room.created_at)},
        {"updated_at", unix_seconds(snapshot.room.updated_at)},
        {"member_count", static_cast<std::int64_t>(snapshot.members.size())},
        {"members", std::move(members)},
    };
}

}  // namespace

void register_multiplayer_routes(
    mf::core::Application& app,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::multiplayer::MultiplayerRoomService>& multiplayer_service) {
    app.route(mf::http::beast_http::verb::post, "/api/multiplayer/rooms/create",
        [token_service, multiplayer_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            const auto result = multiplayer_service->create_room(
                session->player_id,
                session->nickname,
                read_string_field(object, "room_name"),
                read_optional_i64_field(object, "max_players", 4));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            mf::core::add_current_span_attribute("multiplayer.room_id", result.value->room.room_id);
            return mf::core::make_ok_response(snapshot_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/multiplayer/rooms/join",
        [token_service, multiplayer_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto room_id = read_string_field(body->as_object(), "room_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("multiplayer.room_id", room_id);
            const auto result = multiplayer_service->join_room(room_id, session->player_id, session->nickname);
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(snapshot_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/multiplayer/rooms/get",
        [token_service, multiplayer_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto room_id = read_string_field(body->as_object(), "room_id");
            const auto result = multiplayer_service->get_room(room_id, session->player_id);
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(snapshot_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/multiplayer/rooms/leave",
        [token_service, multiplayer_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto room_id = read_string_field(body->as_object(), "room_id");
            const auto result = multiplayer_service->leave_room(room_id, session->player_id);
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(snapshot_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/multiplayer/rooms/close",
        [token_service, multiplayer_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto room_id = read_string_field(body->as_object(), "room_id");
            const auto result = multiplayer_service->close_room(room_id, session->player_id);
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(snapshot_json(*result.value), request.version());
        });
}
