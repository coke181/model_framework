#include "mf/voice/types.hpp"

#include <cstdlib>
#include <algorithm>

// 文件职责：从环境变量加载 LiveKit 地址、密钥、Token TTL 和房间前缀。

namespace mf::voice {
namespace {

std::optional<std::string> read_env(const char* primary, const char* fallback) {
#ifdef _WIN32
    for (const char* name : {primary, fallback}) {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
            std::string result{value};
            std::free(value);
            return result;
        }
    }
#else
    if (const char* value = std::getenv(primary)) return std::string{value};
    if (const char* value = std::getenv(fallback)) return std::string{value};
#endif
    return std::nullopt;
}

}  // namespace

VoiceOptions load_voice_options_from_env(VoiceOptions defaults) {
    defaults.server_url = read_env("HELLO_SERVICE_LIVEKIT_URL", "LIVEKIT_URL").value_or(defaults.server_url);
    defaults.api_key = read_env("HELLO_SERVICE_LIVEKIT_API_KEY", "LIVEKIT_API_KEY").value_or(defaults.api_key);
    defaults.api_secret = read_env("HELLO_SERVICE_LIVEKIT_API_SECRET", "LIVEKIT_API_SECRET").value_or(defaults.api_secret);
    defaults.room_prefix = read_env("HELLO_SERVICE_VOICE_ROOM_PREFIX", "VOICE_ROOM_PREFIX").value_or(defaults.room_prefix);
    if (const auto ttl = read_env("HELLO_SERVICE_VOICE_TOKEN_TTL_SECONDS", "VOICE_TOKEN_TTL_SECONDS")) {
        try {
            defaults.token_ttl = std::chrono::seconds{std::clamp<std::int64_t>(std::stoll(*ttl), 60, 3600)};
        } catch (...) {
        }
    }
    return defaults;
}

}  // namespace mf::voice
