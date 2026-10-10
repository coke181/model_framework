#pragma once

// 文件职责：定义语音房间成员、审核记录、LiveKit 配置和签发结果。
// 调用流程：HTTP 路由 -> VoiceService -> VoiceRepository/LiveKitTokenProvider -> 返回 VoiceCredentials。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mf::voice {

struct VoiceOptions {
    std::string server_url;
    std::string api_key;
    std::string api_secret;
    std::chrono::seconds token_ttl{900};
    std::string room_prefix = "blaster";
};

struct VoiceRoomMember {
    std::string scope_type;
    std::string scope_id;
    std::string room_id;
    std::string player_id;
    std::string display_name;
    std::string role = "member";
    bool server_muted = false;
    bool banned = false;
    std::optional<std::chrono::system_clock::time_point> joined_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct VoiceModerationEvent {
    std::string event_id;
    std::string room_id;
    std::string target_player_id;
    std::string operator_id;
    std::string action;
    std::string reason;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

struct VoiceCredentials {
    std::string server_url;
    std::string room_id;
    std::string participant_id;
    std::string display_name;
    std::string access_token;
    bool can_publish = true;
    std::chrono::system_clock::time_point expires_at;
};

VoiceOptions load_voice_options_from_env(VoiceOptions defaults = {});

}  // namespace mf::voice
