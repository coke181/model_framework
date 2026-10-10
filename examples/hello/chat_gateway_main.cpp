#include "mf/auth/token_service.hpp"
#include "mf/chat/gateway.hpp"
#include "mf/core/logger.hpp"
#include "mf/core/metrics.hpp"
#include "mf/core/tracing.hpp"
#include "mf/storage/factory.hpp"
#include "mf/storage/options.hpp"
#include "mf/storage/redis_repositories.hpp"

#include <cstdlib>
#include <thread>

namespace {

std::string read_env_string(const char* name, std::string fallback) {
    if (const char* value = std::getenv(name)) {
        return std::string{value};
    }
    return fallback;
}

std::uint16_t read_env_port(const char* name, std::uint16_t fallback) {
    if (const char* value = std::getenv(name)) {
        try {
            return static_cast<std::uint16_t>(std::stoi(value));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::size_t read_env_size_t(const char* name, std::size_t fallback) {
    if (const char* value = std::getenv(name)) {
        try {
            const auto parsed = static_cast<std::size_t>(std::stoull(value));
            return parsed == 0 ? fallback : parsed;
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

}  // namespace

int main() {
    // 调用流程：
    // 1. 读取存储配置，构造聊天网关需要的 repositories / token_service / chat services。
    // 2. 单独启动 WebSocket ChatGateway，不再和 HTTP API 进程混跑。
    // 3. 后续所有长连接、心跳、广播、历史拉取都由这个独立进程负责。
    const auto storage_options = mf::storage::load_storage_options_from_env(
        "HELLO_SERVICE",
        mf::storage::StorageOptions{
            .mode = mf::storage::StorageMode::in_memory,
        });
    const auto logger = mf::core::make_console_logger();
    const auto service_name = std::string{"ue-game-chat-gateway"};
    const auto tracer = mf::core::make_tracer_provider(logger, service_name);
    mf::core::configure_global_tracer(tracer);
    const auto metrics = mf::core::make_metrics_registry(service_name);

    const auto repositories = mf::storage::make_repositories(storage_options, logger, metrics);
    const auto token_service = std::make_shared<mf::auth::TokenService>(repositories.sessions);
    const std::shared_ptr<mf::chat::ChatStateService> chat_state_service =
        storage_options.mode == mf::storage::StorageMode::mysql_redis
            ? mf::storage::make_redis_chat_state_service(storage_options.redis)
            : mf::chat::make_in_memory_chat_state_service(read_env_size_t("HELLO_CHAT_STATE_SHARD_COUNT", 64));
    const auto chat_service = std::make_shared<mf::chat::ChatService>(
        repositories.chat,
        chat_state_service,
        repositories.friends,
        repositories.chat_moderation,
        read_env_size_t("HELLO_CHAT_PERSISTENCE_QUEUE_LIMIT", 4096),
        read_env_size_t("HELLO_CHAT_PERSISTENCE_BATCH_SIZE", 64),
        metrics,
        logger,
        std::min<std::size_t>(read_env_size_t("HELLO_CHAT_PERSISTENCE_MAX_RETRIES", 5), 10));

    const auto gateway_host = read_env_string("HELLO_CHAT_GATEWAY_HOST", "127.0.0.1");
    const auto gateway_port = read_env_port("HELLO_CHAT_GATEWAY_PORT", 8090);
    const auto metrics_host = read_env_string("HELLO_CHAT_GATEWAY_METRICS_HOST", "127.0.0.1");
    const auto metrics_port = read_env_port("HELLO_CHAT_GATEWAY_METRICS_PORT", 9091);
    const mf::chat::GatewayOptions chat_options{
        .host = boost::asio::ip::make_address(gateway_host),
        .port = gateway_port,
        .metrics_host = boost::asio::ip::make_address(metrics_host),
        .metrics_port = metrics_port,
        .thread_count = std::max<std::size_t>(1, std::thread::hardware_concurrency()),
        .max_connections = read_env_size_t("HELLO_CHAT_GATEWAY_MAX_CONNECTIONS", 10000),
        .max_message_bytes = read_env_size_t("HELLO_CHAT_GATEWAY_MAX_MESSAGE_BYTES", 64 * 1024),
        .max_outbox_bytes_per_connection =
            read_env_size_t("HELLO_CHAT_GATEWAY_MAX_OUTBOX_BYTES", 1024 * 1024),
        .max_outbox_messages_per_connection =
            read_env_size_t("HELLO_CHAT_GATEWAY_MAX_OUTBOX_MESSAGES", 256),
    };

    mf::chat::ChatGateway gateway{
        chat_options,
        chat_service,
        chat_state_service,
        token_service,
        logger,
        metrics,
    };

    logger->log(
        mf::core::LogLevel::info,
        "hello_chat_gateway starting host=" + chat_options.host.to_string() +
            " port=" + std::to_string(chat_options.port));
    gateway.run();
    return 0;
}
