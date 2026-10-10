#pragma once

// 文件职责：只负责按照 LiveKit AccessToken 规范生成短期 HS256 JWT，不处理成员权限判断。

#include "mf/voice/types.hpp"

namespace mf::voice {

class LiveKitTokenProvider {
public:
    explicit LiveKitTokenProvider(VoiceOptions options);

    bool configured() const;
    const VoiceOptions& options() const;
    VoiceCredentials issue(
        const std::string& room_id,
        const std::string& participant_id,
        const std::string& display_name,
        bool can_publish) const;

private:
    VoiceOptions options_;
};

}  // namespace mf::voice
