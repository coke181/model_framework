#pragma once

// 文件职责：
// 定义 MySQL + Redis 存储模式所需的配置对象。
// 调用流程：
// main() 或配置模块构造 StorageOptions ->
// 仓储工厂读取 mode/mysql/redis 配置 ->
// 决定创建内存版仓储还是 MySQL + Redis 生产版仓储。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mf::storage {

enum class StorageMode {
    in_memory,
    mysql_redis,
};

struct MySqlOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string username = "game";
    std::string password = "game";
    std::string database = "game_backend";
    std::size_t pool_size = 16;
    std::uint32_t acquire_timeout_ms = 1000;
    bool auto_migrate = true;
    std::string schema_file = "db/mysql/001_game_backend.sql";
};

struct RedisAddress {
    std::string host;
    std::uint16_t port = 6379;
};

struct RedisOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string password;
    std::uint32_t database_index = 0;
    std::size_t pool_size = 32;

    // 主从 + Sentinel 模式配置。master_name 非空时优先通过 Sentinel 发现 master。
    std::vector<RedisAddress> sentinel_addresses;
    std::string sentinel_master_name;
};

struct StorageOptions {
    StorageMode mode = StorageMode::in_memory;
    MySqlOptions mysql;
    RedisOptions redis;
    // 本机高吞吐压测可把 Session 放进程内，避免登录成功同步写 Redis。
    // 进程重启后内存 token 会失效；生产环境默认保持 false 使用 Redis。
    bool in_memory_sessions = false;
    // 本机纯吞吐压测可把登录风控放进程内，避免 Redis 网络往返。
    // 生产环境必须保持 false，确保多实例共享限流、黑名单和验证码状态。
    bool in_memory_auth_security = false;
};

const char* to_string(StorageMode mode);

}  // namespace mf::storage 命名空间
