#include "hello_admin_tasks.hpp"

#include "mf/auth/account_service.hpp"
#include "mf/auth/password_hasher.hpp"
#include "mf/auth/token_service.hpp"
#include "mf/core/logger.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/mail_service.hpp"
#include "mf/storage/factory.hpp"
#include "mf/storage/options.hpp"
#include "mf/storage/redis_repositories.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::size_t read_chat_state_shard_count() {
    if (const char* value = std::getenv("HELLO_CHAT_STATE_SHARD_COUNT")) {
        try {
            const auto parsed = static_cast<std::size_t>(std::stoull(value));
            return parsed == 0 ? 64 : parsed;
        } catch (...) {
            return 64;
        }
    }
    return 64;
}

}  // namespace

int main() {
    // 调用流程：
    // 1. 读取存储配置，构造 worker 需要的 repositories / services。
    // 2. 创建 AdminTaskScheduler，但不挂 HTTP 路由，也不启动聊天网关。
    // 3. 在当前进程内专门跑后台任务轮询，让 claim/执行不再和在线请求抢资源。
    const auto storage_options = mf::storage::load_storage_options_from_env(
        "HELLO_SERVICE",
        mf::storage::StorageOptions{
            .mode = mf::storage::StorageMode::in_memory,
        });
    const auto logger = mf::core::make_console_logger();
    const auto service_name = std::string{"ue-game-task-worker"};
    const auto metrics = mf::core::make_metrics_registry(service_name);
    const auto tracer = mf::core::make_tracer_provider(logger, service_name);
    mf::core::configure_global_tracer(tracer);

    const auto repositories = mf::storage::make_repositories(storage_options, logger, metrics);
    const auto token_service = std::make_shared<mf::auth::TokenService>(repositories.sessions);
    const auto password_hasher = std::make_shared<mf::auth::PasswordHasher>();
    const auto account_service = std::make_shared<mf::auth::AccountService>(
        repositories.accounts,
        repositories.players,
        repositories.auth_security,
        password_hasher,
        token_service);
    const std::shared_ptr<mf::chat::ChatStateService> chat_state_service =
        storage_options.mode == mf::storage::StorageMode::mysql_redis
            ? mf::storage::make_redis_chat_state_service(storage_options.redis)
            : mf::chat::make_in_memory_chat_state_service(read_chat_state_shard_count());
    const auto chat_service = std::make_shared<mf::chat::ChatService>(
        repositories.chat,
        chat_state_service,
        repositories.friends,
        repositories.chat_moderation,
        4096,
        64,
        metrics,
        logger);

    const auto scheduler = make_admin_task_scheduler(
        chat_service,
        account_service,
        repositories,
        repositories.audit_logs,
        repositories.admin_tasks,
        metrics,
        logger);

    logger->log(mf::core::LogLevel::info, "hello_task_worker started");
    std::jthread worker([scheduler](std::stop_token stop_token) {
        scheduler->run_until_stopped(stop_token);
    });
    worker.join();
    return 0;
}
