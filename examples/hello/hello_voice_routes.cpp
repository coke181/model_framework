#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"

#include <boost/json/serialize.hpp>

#include <chrono>
#include <vector>

// 文件职责：提供语音 Token、成员查询、管理员成员登记和审核接口。
// 调用流程：游戏 Bearer Token/管理员权限 -> VoiceService -> 成员仓储与 LiveKitTokenProvider。

namespace {

std::string read_scope_type(const boost::json::object& object) {
    if (object.if_contains("scope_type")) return read_string_field(object, "scope_type");
    return read_string_field(object, "scopeType");
}

std::string read_scope_id(const boost::json::object& object) {
    if (object.if_contains("scope_id")) return read_string_field(object, "scope_id");
    return read_string_field(object, "scopeId");
}

boost::json::object member_json(const mf::voice::VoiceRoomMember& member) {
    return {
        {"scope_type", member.scope_type}, {"scope_id", member.scope_id}, {"room_id", member.room_id},
        {"player_id", member.player_id}, {"display_name", member.display_name}, {"role", member.role},
        {"server_muted", member.server_muted}, {"banned", member.banned},
    };
}

boost::json::array members_json(const std::vector<mf::voice::VoiceRoomMember>& members) {
    boost::json::array array;
    for (const auto& member : members) array.push_back(member_json(member));
    return array;
}

boost::json::object credentials_json(const mf::voice::VoiceCredentials& credentials) {
    return {
        {"server_url", credentials.server_url}, {"room_id", credentials.room_id},
        {"participant_id", credentials.participant_id}, {"display_name", credentials.display_name},
        {"access_token", credentials.access_token}, {"can_publish", credentials.can_publish},
        {"expires_at", std::chrono::duration_cast<std::chrono::seconds>(credentials.expires_at.time_since_epoch()).count()},
    };
}

}  // namespace

void register_voice_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::voice::VoiceService>& voice_service) {
    app.route(mf::http::beast_http::verb::post, "/api/voice/token",
        [token_service, voice_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            const auto result = voice_service->issue_token(
                session->player_id, session->nickname, read_scope_type(object), read_scope_id(object));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(credentials_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/voice/rooms/members",
        [token_service, voice_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto result = voice_service->list_members(
                session->player_id, read_scope_type(object), read_scope_id(object));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(
                boost::json::object{{"members", members_json(*result.value)}}, request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/admin/voice/members/upsert",
        [repositories, token_service, voice_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request, *token_service, repositories.admins, "admin.voice_manage",
                session_storage, admin_storage, error);
            if (!admin) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto player_id = read_string_field(object, "player_id");
            const auto player = repositories.players->find_by_player_id(player_id);
            if (!player) return mf::core::make_error_response(
                {.status = boost::beast::http::status::not_found, .code = "player_not_found", .message = "目标玩家不存在"},
                request.version());
            auto role = read_optional_string_field(object, "role");
            if (role.empty()) role = "member";
            const auto result = voice_service->upsert_member(
                read_scope_type(object), read_scope_id(object), player_id, player->nickname, role);
            append_admin_audit(
                repositories.audit_logs, request, admin->operator_name, "voice_member_upsert", "player",
                player_id, boost::json::serialize(object), !result.error,
                result.error ? result.error->message : std::string{});
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(member_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/admin/voice/moderation",
        [repositories, token_service, voice_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request, *token_service, repositories.admins, "admin.voice_manage",
                session_storage, admin_storage, error);
            if (!admin) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto target_player_id = read_string_field(object, "target_player_id");
            const auto action = read_string_field(object, "action");
            const auto result = voice_service->moderate(
                read_scope_type(object), read_scope_id(object), target_player_id, admin->operator_name,
                action, read_optional_string_field(object, "reason"));
            append_admin_audit(
                repositories.audit_logs, request, admin->operator_name, "voice_" + action, "player",
                target_player_id, boost::json::serialize(object), !result.error,
                result.error ? result.error->message : std::string{});
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(member_json(*result.value), request.version());
        });
}
