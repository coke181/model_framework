#pragma once

// 文件职责：定义权威多人房间、房间成员和仓储变更结果。
// 调用流程：HTTP 路由 -> MultiplayerRoomService -> MultiplayerRoomRepository -> MySQL/内存实现。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mf::multiplayer {

struct MultiplayerRoom {
    std::string room_id;
    std::string owner_player_id;
    std::string room_name;
    std::int64_t max_players = 4;
    std::string status = "open";
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct MultiplayerRoomMember {
    std::string room_id;
    std::string player_id;
    std::string display_name;
    std::string role = "member";
    std::optional<std::chrono::system_clock::time_point> joined_at;
};

struct MultiplayerRoomSnapshot {
    MultiplayerRoom room;
    std::vector<MultiplayerRoomMember> members;
};

enum class RoomMutationStatus {
    success,
    room_exists,
    room_not_found,
    room_closed,
    room_full,
    already_member,
    member_not_found,
    owner_must_close,
    permission_denied,
};

// 仓储在一次加锁或数据库事务中返回变更结果。
// affected_members 用于关闭房间后批量清理对应的语音成员。
struct RoomMutationResult {
    RoomMutationStatus status = RoomMutationStatus::success;
    std::optional<MultiplayerRoom> room;
    std::optional<MultiplayerRoomMember> member;
    std::vector<MultiplayerRoomMember> affected_members;
};

}  // namespace mf::multiplayer
