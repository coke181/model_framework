#pragma once

// 文件职责：隔离语音房间成员和审核记录的内存/MySQL 实现。

#include "mf/voice/types.hpp"

#include <memory>

namespace mf::voice {

class VoiceRepository {
public:
    virtual ~VoiceRepository() = default;

    virtual VoiceRoomMember upsert_member(VoiceRoomMember member) = 0;
    virtual std::optional<VoiceRoomMember> find_member(
        const std::string& room_id,
        const std::string& player_id) const = 0;
    virtual std::vector<VoiceRoomMember> list_members(const std::string& room_id) const = 0;
    virtual bool remove_member(
        const std::string& room_id,
        const std::string& player_id) = 0;
    virtual std::optional<VoiceRoomMember> set_moderation(
        const std::string& room_id,
        const std::string& player_id,
        bool server_muted,
        bool banned) = 0;
    virtual VoiceModerationEvent append_moderation_event(VoiceModerationEvent event) = 0;
};

}  // namespace mf::voice
