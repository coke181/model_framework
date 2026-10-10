#pragma once

// 文件职责：
// 定义日志接口和默认控制台日志构造函数。
// 调用流程：
// Application 或其他组件持有 Logger -> 在请求处理、启动、异常等位置调用 log() ->
// 具体实现决定日志最终写到控制台、文件或外部系统。

#include <memory>
#include <string_view>

namespace mf::core {

enum class LogLevel {
    debug,
    info,
    warn,
    error,
};

class Logger {
public:
    virtual ~Logger() = default;

    // 日志接口保持抽象，
    // 这样框架可以先从 stdout 起步，之后再替换为 spdlog、JSON 结构化日志、
    // tracing 后端或游戏运维侧的日志接收器。
    virtual void log(LogLevel level, std::string_view message) = 0;
};

std::shared_ptr<Logger> make_console_logger();
const char* to_string(LogLevel level);

}  // namespace mf::core 命名空间
