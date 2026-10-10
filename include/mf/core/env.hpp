#pragma once

// 文件职责：
// 定义从环境变量加载应用配置的入口。
// 调用流程：
// main() 先准备默认 AppOptions -> 调用 load_app_options_from_env() ->
// 读取环境变量覆盖默认值 -> 返回最终配置给 Application。

#include "mf/core/application.hpp"

#include <string>

namespace mf::core {

enum class RuntimeEnvironment {
    development,
    staging,
    production,
};

// 把运行环境转成字符串，主要用于日志、诊断接口和文档说明。
std::string to_string(RuntimeEnvironment environment);

// 解析运行环境字符串。
// 支持 development/dev、staging/stage、production/prod；未知值回退到 fallback。
RuntimeEnvironment parse_runtime_environment(
    const std::string& value,
    RuntimeEnvironment fallback = RuntimeEnvironment::development);

// 从 PREFIX_ENV 读取运行环境。
// hello_service 使用 HELLO_SERVICE_ENV；建议服务器显式配置为 production。
RuntimeEnvironment load_runtime_environment_from_env(
    std::string env_prefix,
    RuntimeEnvironment fallback = RuntimeEnvironment::development);

bool is_development(RuntimeEnvironment environment);
bool is_production(RuntimeEnvironment environment);

// 从 PREFIX_NAME / PREFIX_HOST / PREFIX_PORT / PREFIX_THREADS / PREFIX_ACCEPT_CONCURRENCY / PREFIX_BUSINESS_THREADS /
// PREFIX_MAX_INFLIGHT_REQUESTS / PREFIX_MAX_PENDING_BUSINESS_TASKS /
// PREFIX_MAX_PENDING_TASKS_PER_KEY / PREFIX_SERIALIZE_MUTATING_REQUESTS / PREFIX_SLOW_REQUEST_MS 读取进程配置，
// 同时保留调用方传入且未被覆盖的默认值。
AppOptions load_app_options_from_env(
    std::string env_prefix,
    AppOptions defaults = {});

}  // namespace mf::core 命名空间
