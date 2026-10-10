#pragma once

// 文件职责：
// 定义 WebSocket 聊天网关入口，负责监听、接入和会话派发。
// 调用流程：
// main() 构造 ChatGateway -> 调用 run() 监听端口 ->
// Listener 接收连接 -> 为每个连接创建 GatewaySession ->
// GatewaySession 完成鉴权、入房、发言、拉历史等协议流程。

#include "mf/auth/token_service.hpp"
#include "mf/chat/chat_service.hpp"
#include "mf/chat/state.hpp"
#include "mf/core/logger.hpp"
#include "mf/core/metrics.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace mf::chat {

struct GatewayOptions {
    // Chat gateway 独立于 HTTP API 运行，
    // 这样实时流量在扩容、部署、故障隔离上都能和请求响应接口相对分开。
    boost::asio::ip::address host = boost::asio::ip::make_address("0.0.0.0");
    unsigned short port = 8090;
    // 指标默认仅绑定回环地址，避免把连接数、存储池状态暴露到公网。
    boost::asio::ip::address metrics_host = boost::asio::ip::make_address("127.0.0.1");
    unsigned short metrics_port = 9091;
    std::size_t thread_count = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    std::size_t max_connections = 10000;
    std::size_t max_message_bytes = 64 * 1024;
    std::size_t max_outbox_bytes_per_connection = 1024 * 1024;
    std::size_t max_outbox_messages_per_connection = 256;
};

class ChatGateway {
public:
    ChatGateway(
        GatewayOptions options,
        std::shared_ptr<ChatService> chat_service,
        std::shared_ptr<ChatStateService> state_service,
        std::shared_ptr<mf::auth::TokenService> token_service,
        std::shared_ptr<mf::core::Logger> logger,
        std::shared_ptr<mf::core::MetricsRegistry> metrics = {});

    // run() 会阻塞在当前 gateway 实例自己的 websocket 事件循环上。
    void run();

private:
    class Listener;

    GatewayOptions options_;
    std::shared_ptr<ChatService> chat_service_;
    std::shared_ptr<ChatStateService> state_service_;
    std::shared_ptr<mf::auth::TokenService> token_service_;
    std::shared_ptr<mf::core::Logger> logger_;
    std::shared_ptr<mf::core::MetricsRegistry> metrics_;
    boost::asio::io_context io_context_;
};

}  // namespace mf::chat 命名空间
