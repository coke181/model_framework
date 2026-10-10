#pragma once

// 文件职责：隔离多人房间的内存实现和 MySQL 实现。
// 并发要求：create/join/leave/close 必须在单次锁或单个数据库事务内完成校验和写入。

#include "mf/multiplayer/types.hpp"

namespace mf::multiplayer {

class MultiplayerRoomRepository {
public:
    virtual ~MultiplayerRoomRepository() = default;

    virtual RoomMutationResult create_room(
        MultiplayerRoom room,
        MultiplayerRoomMember owner) = 0;
    virtual RoomMutationResult join_room(
        const std::string& room_id,
        MultiplayerRoomMember member) = 0;
    virtual RoomMutationResult leave_room(
        const std::string& room_id,
        const std::string& player_id) = 0;
    virtual RoomMutationResult close_room(
        const std::string& room_id,
        const std::string& requester_player_id) = 0;

    virtual std::optional<MultiplayerRoom> find_room(const std::string& room_id) const = 0;
    virtual std::optional<MultiplayerRoomMember> find_member(
        const std::string& room_id,
        const std::string& player_id) const = 0;
    virtual std::vector<MultiplayerRoomMember> list_members(const std::string& room_id) const = 0;
};

}  // namespace mf::multiplayer
