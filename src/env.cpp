#include "mf/core/env.hpp"

// 文件职责：
// 实现环境变量配置加载。
// 调用流程：
// main() 提供默认配置 -> load_app_options_from_env() 按前缀读取环境变量 ->
// 覆盖名称、地址、端口、网络线程数、业务线程数、背压阈值、慢请求阈值 -> 返回最终 AppOptions。

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace mf::core {
namespace {

std::optional<std::string> read_env(const std::string& name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name.c_str()) == 0 && value != nullptr) {
        std::string result{value};
        std::free(value);
        return result;
    }
#else
    if (const char* value = std::getenv(name.c_str())) {
        return std::string{value};
    }
#endif
    return std::nullopt;
}

std::string read_string(const std::string& name, std::string fallback) {
    return read_env(name).value_or(std::move(fallback));
}

unsigned short read_port(const std::string& name, unsigned short fallback) {
    if (const auto value = read_env(name)) {
        return static_cast<unsigned short>(std::stoi(*value));
    }
    return fallback;
}

std::size_t read_size(const std::string& name, std::size_t fallback) {
    if (const auto value = read_env(name)) {
        const auto parsed = static_cast<std::size_t>(std::stoull(*value));
        return parsed == 0 ? fallback : parsed;
    }
    return fallback;
}

bool read_bool(const std::string& name, bool fallback) {
    if (const auto value = read_env(name)) {
        auto normalized = *value;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes") {
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
            return false;
        }
    }
    return fallback;
}

}  // 匿名命名空间

std::string to_string(RuntimeEnvironment environment) {
    switch (environment) {
    case RuntimeEnvironment::development:
        return "development";
    case RuntimeEnvironment::staging:
        return "staging";
    case RuntimeEnvironment::production:
        return "production";
    }
    return "development";
}

RuntimeEnvironment parse_runtime_environment(const std::string& value, RuntimeEnvironment fallback) {
    auto normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "development" || normalized == "dev" || normalized == "local") {
        return RuntimeEnvironment::development;
    }
    if (normalized == "staging" || normalized == "stage" || normalized == "test") {
        return RuntimeEnvironment::staging;
    }
    if (normalized == "production" || normalized == "prod" || normalized == "release") {
        return RuntimeEnvironment::production;
    }
    return fallback;
}

RuntimeEnvironment load_runtime_environment_from_env(std::string env_prefix, RuntimeEnvironment fallback) {
    if (const auto value = read_env(env_prefix + "_ENV")) {
        return parse_runtime_environment(*value, fallback);
    }
    return fallback;
}

bool is_development(RuntimeEnvironment environment) {
    return environment == RuntimeEnvironment::development;
}

bool is_production(RuntimeEnvironment environment) {
    return environment == RuntimeEnvironment::production;
}

AppOptions load_app_options_from_env(std::string env_prefix, AppOptions defaults) {
    // 这里故意只做一个很小的配置层，
    // 先满足部署期和运行期覆盖需求，但不急着把框架绑定到完整配置系统上。
    defaults.name = read_string(env_prefix + "_NAME", defaults.name);
    defaults.server.host = boost::asio::ip::make_address(
        read_string(env_prefix + "_HOST", defaults.server.host.to_string()));
    defaults.server.port = read_port(env_prefix + "_PORT", defaults.server.port);
    defaults.server.thread_count = read_size(env_prefix + "_THREADS", defaults.server.thread_count);
    defaults.server.accept_concurrency = read_size(
        env_prefix + "_ACCEPT_CONCURRENCY",
        defaults.server.accept_concurrency);
    defaults.server.business_thread_count = read_size(
        env_prefix + "_BUSINESS_THREADS",
        defaults.server.business_thread_count);
    defaults.server.max_inflight_requests = read_size(
        env_prefix + "_MAX_INFLIGHT_REQUESTS",
        defaults.server.max_inflight_requests);
    defaults.server.max_pending_business_tasks = read_size(
        env_prefix + "_MAX_PENDING_BUSINESS_TASKS",
        defaults.server.max_pending_business_tasks);
    defaults.server.max_pending_tasks_per_key = read_size(
        env_prefix + "_MAX_PENDING_TASKS_PER_KEY",
        defaults.server.max_pending_tasks_per_key);
    defaults.server.serialize_mutating_requests = read_bool(
        env_prefix + "_SERIALIZE_MUTATING_REQUESTS",
        defaults.server.serialize_mutating_requests);
    defaults.server.slow_request_threshold_ms = read_size(
        env_prefix + "_SLOW_REQUEST_MS",
        defaults.server.slow_request_threshold_ms);
    return defaults;
}

}  // namespace mf::core 命名空间
