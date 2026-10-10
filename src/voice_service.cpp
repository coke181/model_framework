#include "mf/voice/service.hpp"
#include "mf/multiplayer/repository.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

// 文件职责：实现语音房间命名、成员资格检查、Token 签发和管理员禁言/封禁。

namespace mf::voice {
namespace {

mf::core::ApiError voice_error(
    boost::beast::http::status status,
    std::string code,
    std::string message) {
    return {.status = status, .code = std::move(code), .message = std::move(message)};
}

bool valid_identifier(const std::string& value, std::size_t max_length) {
    return !value.empty() && value.size() <= max_length &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == ':';
           });
}

}  // namespace

VoiceService::VoiceService(
    std::shared_ptr<VoiceRepository> repository,
    std::shared_ptr<LiveKitTokenProvider> token_provider,
    std::shared_ptr<mf::multiplayer::MultiplayerRoomRepository> multiplayer_rooms)
    : repository_(std::move(repository)),
      token_provider_(std::move(token_provider)),
      multiplayer_rooms_(std::move(multiplayer_rooms)) {}

std::optional<std::string> VoiceService::room_id_for(
    const std::string& scope_type,
    const std::string& scope_id) const {
    if (scope_type != "session" && scope_type != "party" && scope_type != "guild") return std::nullopt;
    if (!valid_identifier(scope_id, 96)) return std::nullopt;
    const auto& prefix = token_provider_->options().room_prefix;
    if (!valid_identifier(prefix, 32)) return std::nullopt;
    return prefix + '_' + scope_type + '_' + scope_id;
}

mf::game::ServiceResult<VoiceCredentials> VoiceService::issue_token(
    const std::string& player_id,
    const std::string& display_name,
    const std::string& scope_type,
    const std::string& scope_id) const {
    if (!token_provider_->configured()) {
        return {.error = voice_error(
            boost::beast::http::status::service_unavailable,
            "voice_not_configured",
            "LiveKit 地址、API Key 或 API Secret 尚未配置")};
    }
    const auto room_id = room_id_for(scope_type, scope_id);
    if (!room_id) {
        return {.error = voice_error(
            boost::beast::http::status::bad_request,
            "invalid_voice_scope",
            "scopeType 必须是 session、party 或 guild，scopeId 格式必须合法")};
    }
    // session scope 如果对应权威多人房间，则以多人房间成员表为最终准入依据。
    // 这样即使退出/关闭后的语音清理因进程中断尚未完成，也不会继续签发 Token。
    if (scope_type == "session" && multiplayer_rooms_) {
        if (const auto multiplayer_room = multiplayer_rooms_->find_room(scope_id)) {
            if (multiplayer_room->status != "open") {
                return {.error = voice_error(
                    boost::beast::http::status::forbidden,
                    "voice_room_closed",
                    "对应的多人房间已关闭")};
            }
            if (!multiplayer_rooms_->find_member(scope_id, player_id)) {
                return {.error = voice_error(
                    boost::beast::http::status::forbidden,
                    "voice_membership_required",
                    "当前玩家不是该多人房间的权威成员")};
            }
        }
    }
    const auto member = repository_->find_member(*room_id, player_id);
    if (!member) {
        return {.error = voice_error(
            boost::beast::http::status::forbidden,
            "voice_membership_required",
            "当前玩家不是该语音房间的已登记成员")};
    }
    if (member->banned) {
        return {.error = voice_error(
            boost::beast::http::status::forbidden,
            "voice_banned",
            "当前玩家已被禁止进入该语音房间")};
    }
    return {.value = token_provider_->issue(*room_id, player_id, display_name, !member->server_muted)};
}

mf::game::ServiceResult<std::vector<VoiceRoomMember>> VoiceService::list_members(
    const std::string& requester_player_id,
    const std::string& scope_type,
    const std::string& scope_id) const {
    const auto room_id = room_id_for(scope_type, scope_id);
    if (!room_id) {
        return {.error = voice_error(boost::beast::http::status::bad_request, "invalid_voice_scope", "语音房间范围不合法")};
    }
    if (scope_type == "session" && multiplayer_rooms_) {
        if (const auto multiplayer_room = multiplayer_rooms_->find_room(scope_id)) {
            if (multiplayer_room->status != "open" ||
                !multiplayer_rooms_->find_member(scope_id, requester_player_id)) {
                return {.error = voice_error(
                    boost::beast::http::status::forbidden,
                    "voice_membership_required",
                    "无权查询已关闭或未加入的多人房间语音成员")};
            }
        }
    }
    const auto requester = repository_->find_member(*room_id, requester_player_id);
    if (!requester || requester->banned) {
        return {.error = voice_error(boost::beast::http::status::forbidden, "voice_membership_required", "无权查询该语音房间成员")};
    }
    return {.value = repository_->list_members(*room_id)};
}

mf::game::ServiceResult<VoiceRoomMember> VoiceService::upsert_member(
    const std::string& scope_type,
    const std::string& scope_id,
    const std::string& player_id,
    const std::string& display_name,
    const std::string& role) const {
    const auto room_id = room_id_for(scope_type, scope_id);
    if (!room_id || !valid_identifier(player_id, 64) ||
        (role != "member" && role != "leader" && role != "moderator")) {
        return {.error = voice_error(boost::beast::http::status::bad_request, "invalid_voice_member", "语音成员参数不合法")};
    }
    return {.value = repository_->upsert_member({
        .scope_type = scope_type,
        .scope_id = scope_id,
        .room_id = *room_id,
        .player_id = player_id,
        .display_name = display_name,
        .role = role,
    })};
}

mf::game::ServiceResult<VoiceRoomMember> VoiceService::moderate(
    const std::string& scope_type,
    const std::string& scope_id,
    const std::string& target_player_id,
    const std::string& operator_id,
    const std::string& action,
    const std::string& reason) const {
    const auto room_id = room_id_for(scope_type, scope_id);
    if (!room_id) {
        return {.error = voice_error(boost::beast::http::status::bad_request, "invalid_voice_scope", "语音房间范围不合法")};
    }
    const auto before = repository_->find_member(*room_id, target_player_id);
    if (!before) {
        return {.error = voice_error(boost::beast::http::status::not_found, "voice_member_not_found", "目标语音成员不存在")};
    }
    bool muted = before->server_muted;
    bool banned = before->banned;
    if (action == "mute") muted = true;
    else if (action == "unmute") muted = false;
    else if (action == "ban") banned = true;
    else if (action == "unban") banned = false;
    else {
        return {.error = voice_error(boost::beast::http::status::bad_request, "invalid_voice_action", "action 仅支持 mute、unmute、ban、unban")};
    }
    const auto updated = repository_->set_moderation(*room_id, target_player_id, muted, banned);
    if (!updated) {
        return {.error = voice_error(boost::beast::http::status::not_found, "voice_member_not_found", "目标语音成员不存在")};
    }
    repository_->append_moderation_event({
        .room_id = *room_id,
        .target_player_id = target_player_id,
        .operator_id = operator_id,
        .action = action,
        .reason = reason,
    });
    return {.value = updated};
}

// 从逻辑语音房间移除玩家。
// 多人房间退出或关闭时调用，避免玩家退出后仍能继续申请新的 LiveKit Token。
bool VoiceService::remove_member(
    const std::string& scope_type,
    const std::string& scope_id,
    const std::string& player_id) const {
    const auto room_id = room_id_for(scope_type, scope_id);
    return room_id && repository_->remove_member(*room_id, player_id);
}

}  // namespace mf::voice
