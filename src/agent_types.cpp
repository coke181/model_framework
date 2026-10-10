#include "mf/agent/types.hpp"

#include <algorithm>
#include <cstdlib>

// 文件职责：从后端环境变量加载 Agent 模型、限流、超时和确认配置。

namespace mf::agent {
namespace {

std::optional<std::string> read_env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value) {
        std::string result{value};
        std::free(value);
        return result;
    }
#else
    if (const char* value = std::getenv(name)) return std::string{value};
#endif
    return std::nullopt;
}

bool read_bool(const char* name, bool fallback) {
    const auto value = read_env(name);
    if (!value) return fallback;
    return *value != "0" && *value != "false" && *value != "FALSE" && *value != "off";
}

std::int64_t read_i64(const char* name, std::int64_t fallback) {
    try {
        if (const auto value = read_env(name)) return std::stoll(*value);
    } catch (...) {}
    return fallback;
}

}  // namespace

AgentOptions load_agent_options_from_env(AgentOptions defaults) {
    defaults.enabled = read_bool("HELLO_SERVICE_AGENT_ENABLED", defaults.enabled);
    defaults.provider = read_env("HELLO_SERVICE_AGENT_PROVIDER").value_or(defaults.provider);
    defaults.api_endpoint = read_env("HELLO_SERVICE_AGENT_API_ENDPOINT").value_or(defaults.api_endpoint);
    defaults.api_key = read_env("HELLO_SERVICE_AGENT_API_KEY").value_or(defaults.api_key);
    defaults.model = read_env("HELLO_SERVICE_AGENT_MODEL").value_or(defaults.model);
    defaults.timeout = std::chrono::seconds{std::clamp<std::int64_t>(
        read_i64("HELLO_SERVICE_AGENT_TIMEOUT_SECONDS", defaults.timeout.count()), 1, 60)};
    defaults.max_message_length = static_cast<std::size_t>(std::clamp<std::int64_t>(
        read_i64("HELLO_SERVICE_AGENT_MAX_MESSAGE_LENGTH", defaults.max_message_length), 64, 8000));
    defaults.max_history_messages = static_cast<std::size_t>(std::clamp<std::int64_t>(
        read_i64("HELLO_SERVICE_AGENT_MAX_HISTORY_MESSAGES", defaults.max_history_messages), 2, 30));
    defaults.max_context_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
        read_i64("HELLO_SERVICE_AGENT_MAX_CONTEXT_BYTES", defaults.max_context_bytes), 1024, 32768));
    defaults.daily_request_limit = std::clamp<std::int64_t>(
        read_i64("HELLO_SERVICE_AGENT_DAILY_REQUEST_LIMIT", defaults.daily_request_limit), 1, 10000);
    defaults.confirmation_ttl = std::chrono::seconds{std::clamp<std::int64_t>(
        read_i64("HELLO_SERVICE_AGENT_CONFIRMATION_TTL_SECONDS", defaults.confirmation_ttl.count()), 10, 600)};
    return defaults;
}

}  // namespace mf::agent
