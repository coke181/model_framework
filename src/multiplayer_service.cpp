#include "mf/multiplayer/service.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

// 文件职责：实现权威多人房间业务规则，并把房间成员变化同步到语音成员仓储。

namespace mf::multiplayer {
namespace {

mf::core::ApiError multiplayer_error(
    boost::beast::http::status status,
    std::string code,
    std::string message) {
    return {.status = status, .code = std::move(code), .message = std::move(message)};
}

bool valid_room_id(const std::string& value) {
    return !value.empty() && value.size() <= 96 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == ':';
           });
}

std::string make_room_id() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::ostringstream output;
    output << "room_" << std::hex << std::setw(16) << std::setfill('0') << generator();
    return output.str();
}

mf::core::ApiError mutation_error(RoomMutationStatus status) {
    switch (status) {
    case RoomMutationStatus::room_not_found:
        return multiplayer_error(boost::beast::http::status::not_found, "multiplayer_room_not_found", "多人房间不存在");
    case RoomMutationStatus::room_closed:
        return multiplayer_error(boost::beast::http::status::conflict, "multiplayer_room_closed", "多人房间已关闭，不能继续加入");
    case RoomMutationStatus::room_full:
        return multiplayer_error(boost::beast::http::status::conflict, "multiplayer_room_full", "多人房间人数已满");
    case RoomMutationStatus::already_member:
        return multiplayer_error(boost::beast::http::status::conflict, "multiplayer_already_member", "当前玩家已经在该多人房间中");
    case RoomMutationStatus::member_not_found:
        return multiplayer_error(boost::beast::http::status::not_found, "multiplayer_member_not_found", "当前玩家不是该多人房间成员");
    case RoomMutationStatus::owner_must_close:
        return multiplayer_error(boost::beast::http::status::conflict, "multiplayer_owner_must_close", "房主不能直接退出，请关闭房间");
    case RoomMutationStatus::permission_denied:
        return multiplayer_error(boost::beast::http::status::forbidden, "multiplayer_permission_denied", "只有房主可以关闭多人房间");
    case RoomMutationStatus::room_exists:
        return multiplayer_error(boost::beast::http::status::conflict, "multiplayer_room_id_conflict", "多人房间 ID 冲突，请重试");
    case RoomMutationStatus::success:
        break;
    }
    return multiplayer_error(boost::beast::http::status::internal_server_error, "multiplayer_operation_failed", "多人房间操作失败");
}

}  // namespace

MultiplayerRoomService::MultiplayerRoomService(
    std::shared_ptr<MultiplayerRoomRepository> repository,
    std::shared_ptr<mf::voice::VoiceService> voice_service)
    : repository_(std::move(repository)), voice_service_(std::move(voice_service)) {}

// 创建权威多人房间，并将创建者同时登记为语音房间 leader。
mf::game::ServiceResult<MultiplayerRoomSnapshot> MultiplayerRoomService::create_room(
    const std::string& owner_player_id,
    const std::string& display_name,
    std::string room_name,
    std::int64_t max_players) const {
    if (room_name.empty() || room_name.size() > 128) {
        return {.error = multiplayer_error(
            boost::beast::http::status::bad_request,
            "invalid_multiplayer_room_name",
            "room_name 不能为空且长度不能超过 128 个字符")};
    }
    if (max_players < 2 || max_players > 64) {
        return {.error = multiplayer_error(
            boost::beast::http::status::bad_request,
            "invalid_multiplayer_max_players",
            "max_players 必须在 2 到 64 之间")};
    }

    for (int attempt = 0; attempt < 5; ++attempt) {
        MultiplayerRoom room{
            .room_id = make_room_id(),
            .owner_player_id = owner_player_id,
            .room_name = room_name,
            .max_players = max_players,
            .status = "open",
        };
        MultiplayerRoomMember owner{
            .room_id = room.room_id,
            .player_id = owner_player_id,
            .display_name = display_name,
            .role = "leader",
        };
        auto result = repository_->create_room(room, owner);
        if (result.status == RoomMutationStatus::room_exists) continue;
        if (result.status != RoomMutationStatus::success || !result.room || !result.member) {
            return mutation_result(std::move(result));
        }
        const auto voice = voice_service_->upsert_member(
            "session", result.room->room_id, owner_player_id, display_name, "leader");
        if (voice.error) {
            repository_->close_room(result.room->room_id, owner_player_id);
            return {.error = *voice.error};
        }
        return {.value = snapshot_for(*result.room)};
    }
    return {.error = mutation_error(RoomMutationStatus::room_exists)};
}

// 原子检查房间状态和容量，加入成功后再登记对应语音成员。
mf::game::ServiceResult<MultiplayerRoomSnapshot> MultiplayerRoomService::join_room(
    const std::string& room_id,
    const std::string& player_id,
    const std::string& display_name) const {
    if (!valid_room_id(room_id)) {
        return {.error = multiplayer_error(boost::beast::http::status::bad_request, "invalid_multiplayer_room_id", "room_id 格式不合法")};
    }
    auto result = repository_->join_room(room_id, {
        .room_id = room_id,
        .player_id = player_id,
        .display_name = display_name,
        .role = "member",
    });
    if (result.status != RoomMutationStatus::success || !result.room || !result.member) {
        return mutation_result(std::move(result));
    }
    const auto voice = voice_service_->upsert_member("session", room_id, player_id, display_name, "member");
    if (voice.error) {
        repository_->leave_room(room_id, player_id);
        return {.error = *voice.error};
    }
    return {.value = snapshot_for(*result.room)};
}

// 普通成员退出房间，同时删除语音成员，确保不能继续申请该房间的新 Token。
mf::game::ServiceResult<MultiplayerRoomSnapshot> MultiplayerRoomService::leave_room(
    const std::string& room_id,
    const std::string& player_id) const {
    if (!valid_room_id(room_id)) {
        return {.error = multiplayer_error(boost::beast::http::status::bad_request, "invalid_multiplayer_room_id", "room_id 格式不合法")};
    }
    auto result = repository_->leave_room(room_id, player_id);
    if (result.status != RoomMutationStatus::success || !result.room) return mutation_result(std::move(result));
    voice_service_->remove_member("session", room_id, player_id);
    return {.value = snapshot_for(*result.room)};
}

// 房主关闭房间，并清理所有持久化语音成员。
mf::game::ServiceResult<MultiplayerRoomSnapshot> MultiplayerRoomService::close_room(
    const std::string& room_id,
    const std::string& requester_player_id) const {
    if (!valid_room_id(room_id)) {
        return {.error = multiplayer_error(boost::beast::http::status::bad_request, "invalid_multiplayer_room_id", "room_id 格式不合法")};
    }
    auto result = repository_->close_room(room_id, requester_player_id);
    if (result.status != RoomMutationStatus::success || !result.room) return mutation_result(std::move(result));
    for (const auto& member : result.affected_members) {
        voice_service_->remove_member("session", room_id, member.player_id);
    }
    return {.value = MultiplayerRoomSnapshot{.room = *result.room, .members = {}}};
}

// 只有房间成员可以读取权威房间快照，避免把成员信息暴露给任意已登录玩家。
mf::game::ServiceResult<MultiplayerRoomSnapshot> MultiplayerRoomService::get_room(
    const std::string& room_id,
    const std::string& requester_player_id) const {
    if (!valid_room_id(room_id)) {
        return {.error = multiplayer_error(boost::beast::http::status::bad_request, "invalid_multiplayer_room_id", "room_id 格式不合法")};
    }
    const auto room = repository_->find_room(room_id);
    if (!room) return {.error = mutation_error(RoomMutationStatus::room_not_found)};
    if (!repository_->find_member(room_id, requester_player_id)) {
        return {.error = multiplayer_error(boost::beast::http::status::forbidden, "multiplayer_membership_required", "当前玩家不是该多人房间成员")};
    }
    return {.value = snapshot_for(*room)};
}

mf::game::ServiceResult<MultiplayerRoomSnapshot> MultiplayerRoomService::mutation_result(
    RoomMutationResult result) const {
    if (result.status != RoomMutationStatus::success) return {.error = mutation_error(result.status)};
    if (!result.room) {
        return {.error = multiplayer_error(boost::beast::http::status::internal_server_error, "multiplayer_room_state_missing", "多人房间变更结果缺少房间状态")};
    }
    return {.value = snapshot_for(*result.room)};
}

MultiplayerRoomSnapshot MultiplayerRoomService::snapshot_for(const MultiplayerRoom& room) const {
    return {.room = room, .members = repository_->list_members(room.room_id)};
}

}  // namespace mf::multiplayer
