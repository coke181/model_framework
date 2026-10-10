#include "mf/storage/factory.hpp"

// 文件职责：
// 实现存储工厂与存储环境变量加载。
// 调用流程：
// main() 调用 load_storage_options_from_env() -> 得到当前模式配置 ->
// 再调用 make_repositories() -> 根据 mode 构造 in_memory 或 mysql_redis 仓储组合。

#include "mf/storage/cached_repositories.hpp"
#include "mf/storage/mysql_repositories.hpp"
#include "mf/storage/mysql_schema_migration.hpp"
#include "mf/storage/redis_repositories.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace mf::storage {
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

std::vector<RedisAddress> read_redis_sentinels(const std::string& name) {
    std::vector<RedisAddress> addresses;
    const auto raw = read_env(name);
    if (!raw || raw->empty()) {
        return addresses;
    }

    std::stringstream stream{*raw};
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto colon = item.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        RedisAddress address;
        address.host = item.substr(0, colon);
        address.port = static_cast<std::uint16_t>(std::stoul(item.substr(colon + 1)));
        if (!address.host.empty() && address.port != 0) {
            addresses.push_back(std::move(address));
        }
    }
    return addresses;
}

std::uint16_t read_u16(const std::string& name, std::uint16_t fallback) {
    if (const auto value = read_env(name)) {
        return static_cast<std::uint16_t>(std::stoi(*value));
    }
    return fallback;
}

std::uint32_t read_u32(const std::string& name, std::uint32_t fallback) {
    if (const auto value = read_env(name)) {
        return static_cast<std::uint32_t>(std::stoul(*value));
    }
    return fallback;
}

bool read_bool(const std::string& name, bool fallback) {
    if (const auto value = read_env(name)) {
        return *value != "0" && *value != "false" && *value != "FALSE" && *value != "off" && *value != "OFF";
    }
    return fallback;
}

}  // 匿名命名空间

mf::game::RepositoryBundle make_repositories(
    const StorageOptions& options,
    const std::shared_ptr<mf::core::Logger>& logger,
    const std::shared_ptr<mf::core::MetricsRegistry>& metrics) {
    if (options.mode == StorageMode::in_memory) {
        logger->log(mf::core::LogLevel::info, "using in-memory repositories");
        return mf::game::make_in_memory_repositories();
    }

    logger->log(
        mf::core::LogLevel::info,
        options.in_memory_sessions
            ? "using MySQL + Redis repositories with in-memory sessions"
            : "using MySQL + Redis repositories");
    run_mysql_schema_migration_if_enabled(options.mysql, logger);
    configure_mysql_observability(logger, metrics, 100);
    configure_redis_observability(logger, metrics, 100);
    auto repositories = mf::game::make_in_memory_repositories();
    repositories.accounts = make_mysql_account_repository(options.mysql);
    repositories.agent = make_mysql_agent_repository(options.mysql);
    repositories.players = make_mysql_player_repository(options.mysql);
    repositories.item_catalog = make_mysql_item_catalog_repository(options.mysql);
    repositories.gacha = make_mysql_gacha_repository(options.mysql);
    repositories.gacha_transactions = make_mysql_gacha_transaction_repository(options.mysql);
    repositories.shop = make_mysql_shop_repository(options.mysql);
    repositories.loot = make_mysql_loot_repository(options.mysql);
    repositories.reward_transactions = make_mysql_reward_transaction_repository(options.mysql);
    repositories.loot_transactions = make_mysql_loot_transaction_repository(options.mysql);
    repositories.shop_transactions = make_mysql_shop_transaction_repository(options.mysql);
    repositories.quests = make_mysql_quest_repository(options.mysql);
    repositories.multiplayer_rooms = make_mysql_multiplayer_room_repository(options.mysql);
    repositories.voice = make_mysql_voice_repository(options.mysql);
    repositories.friends = make_mysql_friend_repository(options.mysql);
    repositories.chat_moderation = make_mysql_chat_moderation_repository(options.mysql);
    repositories.rewards = make_mysql_reward_repository(options.mysql);
    repositories.reward_templates = make_cached_reward_template_repository(
        make_mysql_reward_template_repository(options.mysql));
    repositories.announcements = make_redis_cached_announcement_repository(
        options.redis,
        make_mysql_announcement_repository(options.mysql));
    repositories.mails = make_mysql_mail_repository(options.mysql);
    repositories.audit_logs = make_mysql_audit_log_repository(options.mysql);
    repositories.admins = make_cached_admin_repository(make_mysql_admin_repository(options.mysql));
    repositories.admin_tasks = make_mysql_admin_task_repository(options.mysql);
    repositories.chat = make_redis_cached_chat_repository(
        options.redis,
        make_mysql_chat_repository(options.mysql));
    repositories.sessions = options.in_memory_sessions
        ? mf::game::make_in_memory_repositories().sessions
        : make_redis_session_repository(options.redis);
    repositories.auth_security = options.in_memory_auth_security
        ? mf::game::make_in_memory_repositories().auth_security
        : make_redis_auth_security_repository(options.redis);
    return repositories;
}

StorageOptions load_storage_options_from_env(std::string env_prefix, StorageOptions defaults) {
    const auto mode = read_string(env_prefix + "_STORAGE_MODE", to_string(defaults.mode));
    defaults.mode = (mode == "mysql_redis") ? StorageMode::mysql_redis : StorageMode::in_memory;
    defaults.in_memory_sessions = read_bool(
        env_prefix + "_IN_MEMORY_SESSIONS",
        defaults.in_memory_sessions);
    defaults.in_memory_auth_security = read_bool(
        env_prefix + "_IN_MEMORY_AUTH_SECURITY",
        defaults.in_memory_auth_security);

    defaults.mysql.host = read_string(env_prefix + "_MYSQL_HOST", defaults.mysql.host);
    defaults.mysql.port = read_u16(env_prefix + "_MYSQL_PORT", defaults.mysql.port);
    defaults.mysql.username = read_string(env_prefix + "_MYSQL_USER", defaults.mysql.username);
    defaults.mysql.password = read_string(env_prefix + "_MYSQL_PASSWORD", defaults.mysql.password);
    defaults.mysql.database = read_string(env_prefix + "_MYSQL_DATABASE", defaults.mysql.database);
    defaults.mysql.pool_size = read_u32(env_prefix + "_MYSQL_POOL_SIZE", static_cast<std::uint32_t>(defaults.mysql.pool_size));
    defaults.mysql.acquire_timeout_ms = read_u32(
        env_prefix + "_MYSQL_ACQUIRE_TIMEOUT_MS",
        defaults.mysql.acquire_timeout_ms);
    defaults.mysql.auto_migrate = read_bool(env_prefix + "_MYSQL_AUTO_MIGRATE", defaults.mysql.auto_migrate);
    defaults.mysql.schema_file = read_string(env_prefix + "_MYSQL_SCHEMA_FILE", defaults.mysql.schema_file);

    defaults.redis.host = read_string(env_prefix + "_REDIS_HOST", defaults.redis.host);
    defaults.redis.port = read_u16(env_prefix + "_REDIS_PORT", defaults.redis.port);
    defaults.redis.password = read_string(env_prefix + "_REDIS_PASSWORD", defaults.redis.password);
    defaults.redis.database_index = read_u32(env_prefix + "_REDIS_DB", defaults.redis.database_index);
    defaults.redis.pool_size = read_u32(env_prefix + "_REDIS_POOL_SIZE", static_cast<std::uint32_t>(defaults.redis.pool_size));
    defaults.redis.sentinel_master_name = read_string(
        env_prefix + "_REDIS_SENTINEL_MASTER_NAME",
        defaults.redis.sentinel_master_name);
    defaults.redis.sentinel_addresses = read_redis_sentinels(env_prefix + "_REDIS_SENTINELS");
    return defaults;
}

}  // namespace mf::storage 命名空间
