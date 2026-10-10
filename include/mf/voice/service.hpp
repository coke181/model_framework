#pragma once

// 文件职责：执行语音 scope 校验、成员权限检查、Token 签发和管理员审核流程。
// 调用流程：语音路由完成游戏 Token 鉴权 -> VoiceService -> VoiceRepository/LiveKitTokenProvider。

#include "mf/core/api.hpp"
#include "mf/game/mail_service.hpp"
#include "mf/voice/livekit_token_provider.hpp"
#include "mf/voice/repository.hpp"

namespace mf::multiplayer {
class MultiplayerRoomRepository;
}

namespace mf::voice {

class VoiceService {
public:
    VoiceService(
        std::shared_ptr<VoiceRepository> repository,
        std::shared_ptr<LiveKitTokenProvider> token_provider,
        std::shared_ptr<mf::multiplayer::MultiplayerRoomRepository> multiplayer_rooms = {});

    mf::game::ServiceResult<VoiceCredentials> issue_token(
        const std::string& player_id,
        const std::string& display_name,
        const std::string& scope_type,
        const std::string& scope_id) const;
    mf::game::ServiceResult<std::vector<VoiceRoomMember>> list_members(
        const std::string& requester_player_id,
        const std::string& scope_type,
        const std::string& scope_id) const;
    mf::game::ServiceResult<VoiceRoomMember> upsert_member(
        const std::string& scope_type,
        const std::string& scope_id,
        const std::string& player_id,
        const std::string& display_name,
        const std::string& role) const;
    mf::game::ServiceResult<VoiceRoomMember> moderate(
        const std::string& scope_type,
        const std::string& scope_id,
        const std::string& target_player_id,
        const std::string& operator_id,
        const std::string& action,
        const std::string& reason) const;
    bool remove_member(
        const std::string& scope_type,
        const std::string& scope_id,
        const std::string& player_id) const;

    std::optional<std::string> room_id_for(const std::string& scope_type, const std::string& scope_id) const;

private:
    std::shared_ptr<VoiceRepository> repository_;
    std::shared_ptr<LiveKitTokenProvider> token_provider_;
    std::shared_ptr<mf::multiplayer::MultiplayerRoomRepository> multiplayer_rooms_;
};

}  // namespace mf::voice
