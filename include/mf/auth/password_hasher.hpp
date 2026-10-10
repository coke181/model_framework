#pragma once

// 文件职责：
// 定义密码哈希服务，负责把明文密码转换成可落库的 Argon2id 哈希串，并提供校验能力。
// 调用流程：
// 注册接口先调用 hash_password() 生成带参数和盐值的密码串 ->
// AccountRepository 持久化该字符串 ->
// 登录接口再调用 verify_password() 对同一算法参数复算并比对。

#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace mf::auth {

struct PasswordHashOptions {
    // 这些参数集中定义，方便以后统一调优内存成本、迭代次数和输出长度。
    // 默认按 OWASP Argon2id 推荐档位（约 19 MiB、2 passes、单线程），
    // 在保留足够抗 GPU 暴破能力的同时把注册/登录延迟控制在可接受范围。
    std::uint32_t memory_cost_kib = 19456;
    std::uint32_t time_cost = 2;
    std::uint32_t parallelism = 1;
    std::uint32_t salt_size = 16;
    std::uint32_t hash_size = 32;
    // 仅用于性能基准：改用 PBKDF2-SHA256(iterations=1)，用于测量去掉 Argon2 成本后的链路上限。
    // 该模式不具备密码存储安全性，main() 会强制要求 PERF_MODE 且禁止 production。
    bool benchmark_fast_hash = false;

    // 登录高峰期同一账号可能在很短时间内重复校验，这里允许把“成功”校验结果
    // 短暂缓存，避免每次都重新跑一遍 Argon2id。失败结果不缓存，保持暴破成本不变。
    // 30 秒覆盖 UE 子系统重复初始化、启动器到游戏进程交接和短线重连。
    // 缓存 key 是进程随机 HMAC(密码哈希, 明文密码)，不保存明文且只缓存成功结果。
    std::uint32_t verification_cache_ttl_ms = 30000;
    std::uint32_t verification_cache_max_entries = 4096;
};

class PasswordHasher {
public:
    explicit PasswordHasher(PasswordHashOptions options = {});
    ~PasswordHasher();

    std::string hash_password(std::string_view password) const;
    bool verify_password(std::string_view password, std::string_view encoded_hash) const;
    // 只查询“成功校验缓存”，不执行 Argon2id。
    bool has_cached_success(std::string_view password, std::string_view encoded_hash) const;
    std::string scheme_name() const;

    static std::string default_scheme_name();
    static bool is_supported_scheme(std::string_view scheme_name);

private:
    struct VerificationCache;

    bool verify_password_uncached(std::string_view password, std::string_view encoded_hash) const;
    bool use_cached_success(
        const std::string& cache_key,
        std::chrono::steady_clock::time_point now) const;

    PasswordHashOptions options_;
    mutable std::unique_ptr<VerificationCache> verification_cache_;
};

}  // namespace mf::auth 命名空间
