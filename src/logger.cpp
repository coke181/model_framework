#include "mf/core/logger.hpp"

// 文件职责：
// 实现默认控制台日志器。
// 调用流程：
// Application 启动时默认构造 ConsoleLogger ->
// HTTP/Chat 等模块调用 Logger::log() ->
// ConsoleLogger 把日志串行写到标准输出。

#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace mf::core {
namespace {

class ConsoleLogger final : public Logger {
public:
    ConsoleLogger()
        : worker_([this](std::stop_token stop_token) {
              drain_loop(stop_token);
          }) {}

    ~ConsoleLogger() override {
        {
            std::scoped_lock lock{mutex_};
            stopping_ = true;
        }
        condition_.notify_all();
    }

    void log(LogLevel level, std::string_view message) override {
        std::scoped_lock lock{mutex_};
        if (queue_.size() >= kMaxQueuedMessages) {
            ++dropped_messages_;
            return;
        }

        queue_.push_back(LogMessage{
            .level = level,
            .message = std::string{message},
        });
        condition_.notify_one();
    }

private:
    struct LogMessage {
        LogLevel level;
        std::string message;
    };

    void drain_loop(std::stop_token stop_token) {
        std::unique_lock lock{mutex_};
        while (true) {
            condition_.wait(lock, [this, &stop_token] {
                return stopping_ || stop_token.stop_requested() || !queue_.empty();
            });

            while (!queue_.empty()) {
                auto message = std::move(queue_.front());
                queue_.pop_front();
                const auto dropped = std::exchange(dropped_messages_, 0);
                lock.unlock();
                if (dropped > 0) {
                    std::cout << "[mf] level=warn message=\"logger dropped_messages=" << dropped << "\""
                              << std::endl;
                }
                std::cout << "[mf] level=" << to_string(message.level) << " message=\"" << message.message << '"'
                          << std::endl;
                lock.lock();
            }

            if ((stopping_ || stop_token.stop_requested()) && queue_.empty()) {
                break;
            }
        }
    }

    static constexpr std::size_t kMaxQueuedMessages = 8192;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<LogMessage> queue_;
    std::jthread worker_;
    std::size_t dropped_messages_ = 0;
    bool stopping_ = false;
};

}  // 匿名命名空间

std::shared_ptr<Logger> make_console_logger() {
    return std::make_shared<ConsoleLogger>();
}

const char* to_string(LogLevel level) {
    switch (level) {
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    }
    return "unknown";
}

}  // namespace mf::core 命名空间
