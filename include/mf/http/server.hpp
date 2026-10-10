#pragma once

// 文件职责：
// 定义 HTTP 监听器和事件循环入口，对外暴露 HttpServer。
// 调用流程：
// Application 调用 HttpServer::run() -> Listener 绑定地址并监听 ->
// 接收 TCP 连接 -> 为每个连接创建 Session -> Session 读请求、调 Router、写响应。

#include "mf/http/router.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace mf::core {
class MetricsRegistry;
class Logger;
}

namespace mf::http {

struct LoadSnapshot {
    std::size_t inflight_requests = 0;
    std::size_t pending_business_tasks = 0;
    std::size_t max_inflight_requests = 0;
    std::size_t max_pending_business_tasks = 0;
    std::uint64_t rejected_too_many_inflight = 0;
    std::uint64_t rejected_business_queue_full = 0;
};

class LoadProbe {
public:
    explicit LoadProbe(
        std::size_t max_inflight_requests = 0,
        std::size_t max_pending_business_tasks = 0)
        : max_inflight_requests_(max_inflight_requests),
          max_pending_business_tasks_(max_pending_business_tasks) {}

    void configure(std::size_t max_inflight_requests, std::size_t max_pending_business_tasks) {
        max_inflight_requests_.store(max_inflight_requests, std::memory_order_relaxed);
        max_pending_business_tasks_.store(max_pending_business_tasks, std::memory_order_relaxed);
    }

    void set_inflight_requests(std::size_t value) {
        inflight_requests_.store(value, std::memory_order_relaxed);
    }

    void set_pending_business_tasks(std::size_t value) {
        pending_business_tasks_.store(value, std::memory_order_relaxed);
    }

    void increment_rejected_too_many_inflight() {
        rejected_too_many_inflight_.fetch_add(1, std::memory_order_relaxed);
    }

    void increment_rejected_business_queue_full() {
        rejected_business_queue_full_.fetch_add(1, std::memory_order_relaxed);
    }

    LoadSnapshot snapshot() const {
        return LoadSnapshot{
            .inflight_requests = inflight_requests_.load(std::memory_order_relaxed),
            .pending_business_tasks = pending_business_tasks_.load(std::memory_order_relaxed),
            .max_inflight_requests = max_inflight_requests_.load(std::memory_order_relaxed),
            .max_pending_business_tasks = max_pending_business_tasks_.load(std::memory_order_relaxed),
            .rejected_too_many_inflight = rejected_too_many_inflight_.load(std::memory_order_relaxed),
            .rejected_business_queue_full = rejected_business_queue_full_.load(std::memory_order_relaxed),
        };
    }

private:
    std::atomic<std::size_t> inflight_requests_{0};
    std::atomic<std::size_t> pending_business_tasks_{0};
    std::atomic<std::size_t> max_inflight_requests_{0};
    std::atomic<std::size_t> max_pending_business_tasks_{0};
    std::atomic<std::uint64_t> rejected_too_many_inflight_{0};
    std::atomic<std::uint64_t> rejected_business_queue_full_{0};
};

inline std::size_t default_io_thread_count() {
    const auto hardware_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    return hardware_threads;
}

inline std::size_t default_business_thread_count() {
    const auto hardware_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    return hardware_threads;
}

struct ServerOptions {
    // 这些是传输层参数。
    // 业务模块通常除了展示或诊断，不应该关心它们。
    boost::asio::ip::address host = boost::asio::ip::make_address("0.0.0.0");
    unsigned short port = 8080;
    // io_context 线程专门负责 accept、读包、写包和回调推进，不再承担重业务逻辑。
    std::size_t thread_count = default_io_thread_count();
    // 同时挂起的 async_accept 数量。0 表示按 I/O 线程数自动选择，
    // 用于吸收瞬时新建连接峰值，避免只有一个 AcceptEx 时监听队列来不及排空。
    std::size_t accept_concurrency = 0;
    // 业务线程池专门负责同步 handler / 仓储调用，避免阻塞网络事件循环。
    std::size_t business_thread_count = default_business_thread_count();
    // 在途请求指“已读到完整请求，但还没把完整响应写完”的请求数，用来防止全局堆积。
    std::size_t max_inflight_requests = 4096;
    // 待执行业务任务指“已接收，但还在业务线程池排队、尚未开始执行”的请求数。
    std::size_t max_pending_business_tasks = 2048;
    // 同一会话（Authorization token）的写请求在业务线程池前按 key 串行化，
    // 避免一个玩家并发写背包/货币时把压力全部压到数据库行锁上。
    bool serialize_mutating_requests = true;
    // 每个会话 key 最多允许排队的待执行写请求数；超过后直接 503，防止单 key 无限堆积。
    std::size_t max_pending_tasks_per_key = 256;
    // 统一的慢请求阈值，供传输层拒绝路径和业务层慢请求指标共用。
    std::size_t slow_request_threshold_ms = 500;
    // 可选的进程级指标注册表。传输层拒绝请求不经过 middleware，需要在这里直接打点。
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    // 可选的运行时负载探针。调试页可直接读取当前 inflight/pending 和拒绝累计值。
    std::shared_ptr<LoadProbe> load_probe;
    // 传输层启动/监听日志也统一走 Logger，避免和业务日志分裂到不同输出路径。
    std::shared_ptr<mf::core::Logger> logger;
};

class HttpServer {
public:
    HttpServer(ServerOptions options, std::shared_ptr<const Router> router);

    // run() 负责当前 server 实例的事件循环生命周期。
    void run();

private:
    class Listener;

    ServerOptions options_;
    std::shared_ptr<const Router> router_;
    boost::asio::io_context io_context_;
};

}  // namespace mf::http 命名空间
