#pragma once

// 文件职责：处理多人房间参数校验、容量和房主规则，并同步语音成员。
// 调用流程：登录鉴权后的 HTTP 路由 -> MultiplayerRoomService -> 房间仓储 + VoiceService。

#include "mf/game/mail_service.hpp"
#include "mf/multiplayer/repository.hpp"
#include "mf/voice/service.hpp"

#include <memory>

namespace mf::multiplayer {

class MultiplayerRoomService {
public:
    MultiplayerRoomService(
        std::shared_ptr<MultiplayerRoomRepository> repository,
        std::shared_ptr<mf::voice::VoiceService> voice_service);

    mf::game::ServiceResult<MultiplayerRoomSnapshot> create_room(
        const std::string& owner_player_id,
        const std::string& display_name,
        std::string room_name,
        std::int64_t max_players) const;
    mf::game::ServiceResult<MultiplayerRoomSnapshot> join_room(
        const std::string& room_id,
        const std::string& player_id,
        const std::string& display_name) const;
    mf::game::ServiceResult<MultiplayerRoomSnapshot> leave_room(
        const std::string& room_id,
        const std::string& player_id) const;
    mf::game::ServiceResult<MultiplayerRoomSnapshot> close_room(
        const std::string& room_id,
        const std::string& requester_player_id) const;
    mf::game::ServiceResult<MultiplayerRoomSnapshot> get_room(
        const std::string& room_id,
        const std::string& requester_player_id) const;

private:
    mf::game::ServiceResult<MultiplayerRoomSnapshot> mutation_result(
        RoomMutationResult result) const;
    MultiplayerRoomSnapshot snapshot_for(const MultiplayerRoom& room) const;

    std::shared_ptr<MultiplayerRoomRepository> repository_;
    std::shared_ptr<mf::voice::VoiceService> voice_service_;
};

}  // namespace mf::multiplayer
