#include "mf/auth/password_hasher.hpp"

// 文件职责：
// 基于 OpenSSL 提供密码哈希与校验能力。
// 调用流程：
// 1. 注册/改密时先调用 hash_password() 生成可持久化哈希串。
// 2. 登录时调用 verify_password() 解析哈希串并按对应算法复算。
// 3. 新环境若支持 OpenSSL Argon2id 就优先用 Argon2id；否则自动回退到 PBKDF2-SHA256，保证旧版 OpenSSL 也能部署。

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mf::auth {
namespace {

constexpr std::string_view kArgon2Prefix = "$argon2id$";
constexpr std::string_view kPbkdf2Prefix = "$pbkdf2-sha256$";
constexpr std::uint32_t kArgon2Version = 19;
constexpr std::uint32_t kPbkdf2Version = 1;
constexpr std::uint32_t kPbkdf2DefaultIterations = 600000;

#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30200000L && \
    defined(OSSL_KDF_PARAM_ARGON2_MEMCOST) && defined(OSSL_KDF_PARAM_ARGON2_LANES) && \
    defined(OSSL_KDF_PARAM_THREADS)
constexpr bool kHasOpenSslArgon2 = true;
#else
constexpr bool kHasOpenSslArgon2 = false;
#endif

struct ParsedArgon2Record {
    std::uint32_t version = kArgon2Version;
    std::uint32_t memory_cost_kib = 0;
    std::uint32_t time_cost = 0;
    std::uint32_t parallelism = 0;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
};

struct ParsedPbkdf2Record {
    std::uint32_t version = kPbkdf2Version;
    std::uint32_t iterations = 0;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
};

std::vector<unsigned char> random_bytes(std::uint32_t size) {
    std::vector<unsigned char> bytes(size);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return bytes;
}

std::string base64_encode(std::span<const unsigned char> input) {
    if (input.empty()) {
        return {};
    }

    std::string output;
    output.resize(4 * ((input.size() + 2) / 3));
    const auto written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(output.data()),
        input.data(),
        static_cast<int>(input.size()));
    output.resize(static_cast<std::size_t>(written));
    return output;
}

std::optional<std::vector<unsigned char>> base64_decode(std::string_view input) {
    if (input.empty()) {
        return std::vector<unsigned char>{};
    }

    if (input.size() % 4 != 0) {
        return std::nullopt;
    }

    std::vector<unsigned char> output((input.size() / 4) * 3);
    const auto written = EVP_DecodeBlock(
        output.data(),
        reinterpret_cast<const unsigned char*>(input.data()),
        static_cast<int>(input.size()));
    if (written < 0) {
        return std::nullopt;
    }

    std::size_t padding = 0;
    if (!input.empty() && input.back() == '=') {
        padding += 1;
    }
    if (input.size() >= 2 && input[input.size() - 2] == '=') {
        padding += 1;
    }

    output.resize(static_cast<std::size_t>(written) - padding);
    return output;
}

std::vector<std::string> split_string(std::string_view input, char delimiter) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= input.size()) {
        const auto end = input.find(delimiter, begin);
        if (end == std::string_view::npos) {
            parts.emplace_back(input.substr(begin));
            break;
        }
        parts.emplace_back(input.substr(begin, end - begin));
        begin = end + 1;
    }
    return parts;
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::uint64_t result = 0;
    for (const unsigned char ch : value) {
        if (!std::isdigit(ch)) {
            return std::nullopt;
        }
        result = result * 10 + static_cast<std::uint64_t>(ch - '0');
        if (result > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(result);
}

std::string hmac_sha256_hex(std::string_view key, std::string_view first, std::string_view second) {
    std::string message;
    message.reserve(first.size() + second.size() + 1);
    message.append(first);
    message.push_back('\0');
    message.append(second);

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (HMAC(
            EVP_sha256(),
            key.data(),
            static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size(),
            digest.data(),
            &digest_size) == nullptr) {
        throw std::runtime_error("HMAC-SHA256 failed");
    }

    constexpr char hex_digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(static_cast<std::size_t>(digest_size) * 2);
    for (unsigned int i = 0; i < digest_size; ++i) {
        output.push_back(hex_digits[digest[i] >> 4]);
        output.push_back(hex_digits[digest[i] & 0x0f]);
    }
    return output;
}

std::vector<unsigned char> derive_pbkdf2_sha256(
    std::string_view password,
    std::span<const unsigned char> salt,
    std::uint32_t iterations,
    std::size_t hash_size) {
    std::vector<unsigned char> output(hash_size);
    if (PKCS5_PBKDF2_HMAC(
            password.data(),
            static_cast<int>(password.size()),
            salt.data(),
            static_cast<int>(salt.size()),
            static_cast<int>(iterations),
            EVP_sha256(),
            static_cast<int>(output.size()),
            output.data()) != 1) {
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    }
    return output;
}

std::vector<unsigned char> derive_argon2id(
    std::string_view password,
    std::span<const unsigned char> salt,
    std::uint32_t memory_cost_kib,
    std::uint32_t time_cost,
    std::uint32_t parallelism,
    std::size_t hash_size) {
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30200000L && \
    defined(OSSL_KDF_PARAM_ARGON2_MEMCOST) && defined(OSSL_KDF_PARAM_ARGON2_LANES) && \
    defined(OSSL_KDF_PARAM_THREADS)
    std::vector<unsigned char> output(hash_size);

    // EVP_KDF_fetch/EVP_KDF_CTX_new 属于算法对象管理，不是一次校验必须
    // 重复做的工作。密码 worker 线程不会并发复用自己的 thread_local 上下文，
    // 因此可以安全复用，减少登录高峰期的 OpenSSL 分配和 provider 查找。
    struct ThreadLocalArgon2Context {
        EVP_KDF* kdf = nullptr;
        EVP_KDF_CTX* context = nullptr;

        ~ThreadLocalArgon2Context() {
            EVP_KDF_CTX_free(context);
            EVP_KDF_free(kdf);
        }

        EVP_KDF_CTX* get() {
            if (context != nullptr) {
                EVP_KDF_CTX_reset(context);
                return context;
            }

            kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
            if (kdf == nullptr) {
                throw std::runtime_error("EVP_KDF_fetch(ARGON2ID) failed");
            }
            context = EVP_KDF_CTX_new(kdf);
            if (context == nullptr) {
                EVP_KDF_free(kdf);
                kdf = nullptr;
                throw std::runtime_error("EVP_KDF_CTX_new failed");
            }
            return context;
        }
    };

    thread_local ThreadLocalArgon2Context thread_context;
    auto* context = thread_context.get();

    const auto password_bytes = std::span{
        reinterpret_cast<const unsigned char*>(password.data()),
        password.size()};

    std::array<OSSL_PARAM, 7> params{
        OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_PASSWORD,
            const_cast<unsigned char*>(password_bytes.data()),
            password_bytes.size()),
        OSSL_PARAM_construct_octet_string(
            OSSL_KDF_PARAM_SALT,
            const_cast<unsigned char*>(salt.data()),
            salt.size()),
        OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ITER, &time_cost),
        OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory_cost_kib),
        OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_ARGON2_LANES, &parallelism),
        OSSL_PARAM_construct_uint(OSSL_KDF_PARAM_THREADS, &parallelism),
        OSSL_PARAM_construct_end(),
    };

    if (EVP_KDF_derive(context, output.data(), output.size(), params.data()) != 1) {
        throw std::runtime_error("EVP_KDF_derive(ARGON2ID) failed");
    }
    return output;
#else
    static_cast<void>(password);
    static_cast<void>(salt);
    static_cast<void>(memory_cost_kib);
    static_cast<void>(time_cost);
    static_cast<void>(parallelism);
    static_cast<void>(hash_size);
    throw std::runtime_error("OpenSSL Argon2id is not available");
#endif
}

std::string encode_argon2_hash_record(
    std::uint32_t memory_cost_kib,
    std::uint32_t time_cost,
    std::uint32_t parallelism,
    std::span<const unsigned char> salt,
    std::span<const unsigned char> hash) {
    std::ostringstream output;
    output << kArgon2Prefix
           << "v=" << kArgon2Version
           << "$m=" << memory_cost_kib
           << ",t=" << time_cost
           << ",p=" << parallelism
           << '$' << base64_encode(salt)
           << '$' << base64_encode(hash);
    return output.str();
}

std::string encode_pbkdf2_hash_record(
    std::uint32_t iterations,
    std::span<const unsigned char> salt,
    std::span<const unsigned char> hash) {
    std::ostringstream output;
    output << kPbkdf2Prefix
           << "v=" << kPbkdf2Version
           << "$i=" << iterations
           << '$' << base64_encode(salt)
           << '$' << base64_encode(hash);
    return output.str();
}

std::optional<ParsedArgon2Record> parse_argon2_hash_record(std::string_view encoded_hash) {
    if (!encoded_hash.starts_with(kArgon2Prefix)) {
        return std::nullopt;
    }

    const auto parts = split_string(encoded_hash.substr(kArgon2Prefix.size()), '$');
    if (parts.size() != 4 || !std::string_view{parts[0]}.starts_with("v=")) {
        return std::nullopt;
    }

    const auto version = parse_u32(std::string_view{parts[0]}.substr(2));
    if (!version || *version != kArgon2Version) {
        return std::nullopt;
    }

    ParsedArgon2Record record;
    record.version = *version;

    const auto param_parts = split_string(parts[1], ',');
    for (const auto& param : param_parts) {
        if (std::string_view{param}.starts_with("m=")) {
            record.memory_cost_kib = parse_u32(std::string_view{param}.substr(2)).value_or(0);
        } else if (std::string_view{param}.starts_with("t=")) {
            record.time_cost = parse_u32(std::string_view{param}.substr(2)).value_or(0);
        } else if (std::string_view{param}.starts_with("p=")) {
            record.parallelism = parse_u32(std::string_view{param}.substr(2)).value_or(0);
        }
    }

    if (record.memory_cost_kib == 0 || record.time_cost == 0 || record.parallelism == 0) {
        return std::nullopt;
    }

    const auto salt = base64_decode(parts[2]);
    const auto hash = base64_decode(parts[3]);
    if (!salt || !hash || salt->empty() || hash->empty()) {
        return std::nullopt;
    }

    record.salt = std::move(*salt);
    record.hash = std::move(*hash);
    return record;
}

std::optional<ParsedPbkdf2Record> parse_pbkdf2_hash_record(std::string_view encoded_hash) {
    if (!encoded_hash.starts_with(kPbkdf2Prefix)) {
        return std::nullopt;
    }

    const auto parts = split_string(encoded_hash.substr(kPbkdf2Prefix.size()), '$');
    if (parts.size() != 4 || !std::string_view{parts[0]}.starts_with("v=") || !std::string_view{parts[1]}.starts_with("i=")) {
        return std::nullopt;
    }

    const auto version = parse_u32(std::string_view{parts[0]}.substr(2));
    const auto iterations = parse_u32(std::string_view{parts[1]}.substr(2));
    if (!version || *version != kPbkdf2Version || !iterations || *iterations == 0) {
        return std::nullopt;
    }

    const auto salt = base64_decode(parts[2]);
    const auto hash = base64_decode(parts[3]);
    if (!salt || !hash || salt->empty() || hash->empty()) {
        return std::nullopt;
    }

    return ParsedPbkdf2Record{
        .version = *version,
        .iterations = *iterations,
        .salt = std::move(*salt),
        .hash = std::move(*hash),
    };
}

}  // 匿名命名空间

struct PasswordHasher::VerificationCache {
    struct Entry {
        std::chrono::steady_clock::time_point expires_at;
        std::list<std::string>::iterator lru_position;
    };

    VerificationCache(std::size_t max_entries, std::chrono::milliseconds ttl)
        : max_entries(max_entries), ttl(ttl) {
        const auto key_bytes = random_bytes(32);
        verification_key.assign(
            reinterpret_cast<const char*>(key_bytes.data()),
            key_bytes.size());
    }

    std::size_t max_entries;
    std::chrono::milliseconds ttl;
    std::string verification_key;
    std::mutex mutex;
    // 按缓存 key 分片，防止同一账号重复计算，同时允许不同账号并行校验。
    std::array<std::mutex, 64> compute_mutexes;
    std::list<std::string> lru;
    std::unordered_map<std::string, Entry> entries;
};

PasswordHasher::~PasswordHasher() = default;

PasswordHasher::PasswordHasher(PasswordHashOptions options)
    : options_(options),
      verification_cache_(std::make_unique<VerificationCache>(
          static_cast<std::size_t>(options.verification_cache_max_entries),
          std::chrono::milliseconds{options.verification_cache_ttl_ms})) {}

std::string PasswordHasher::hash_password(std::string_view password) const {
    const auto salt = random_bytes(options_.salt_size);
    if (options_.benchmark_fast_hash) {
        // 不安全基准模式只用于拆掉 Argon2 成本，观察其余 HTTP/MySQL/Redis 链路上限。
        const auto hash = derive_pbkdf2_sha256(password, salt, 1, options_.hash_size);
        return encode_pbkdf2_hash_record(1, salt, hash);
    }
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30200000L && \
    defined(OSSL_KDF_PARAM_ARGON2_MEMCOST) && defined(OSSL_KDF_PARAM_ARGON2_LANES) && \
    defined(OSSL_KDF_PARAM_THREADS)
        const auto hash = derive_argon2id(
            password,
            salt,
            options_.memory_cost_kib,
            options_.time_cost,
            options_.parallelism,
            options_.hash_size);
        return encode_argon2_hash_record(
            options_.memory_cost_kib,
            options_.time_cost,
            options_.parallelism,
            salt,
            hash);
#else
        const auto hash = derive_pbkdf2_sha256(
            password,
            salt,
            kPbkdf2DefaultIterations,
            options_.hash_size);
        return encode_pbkdf2_hash_record(kPbkdf2DefaultIterations, salt, hash);
#endif
}

bool PasswordHasher::verify_password(std::string_view password, std::string_view encoded_hash) const {
    if (options_.verification_cache_max_entries == 0 || options_.verification_cache_ttl_ms == 0) {
        return verify_password_uncached(password, encoded_hash);
    }

    const auto cache_key = hmac_sha256_hex(verification_cache_->verification_key, encoded_hash, password);
    const auto now = std::chrono::steady_clock::now();

    if (use_cached_success(cache_key, now)) {
        return true;
    }

    const auto compute_index = std::hash<std::string>{}(cache_key) % verification_cache_->compute_mutexes.size();
    std::unique_lock<std::mutex> compute_lock{verification_cache_->compute_mutexes[compute_index]};
    {
        std::lock_guard<std::mutex> lock{verification_cache_->mutex};
        const auto found = verification_cache_->entries.find(cache_key);
        if (found != verification_cache_->entries.end() && found->second.expires_at > now) {
            verification_cache_->lru.erase(found->second.lru_position);
            verification_cache_->lru.push_front(cache_key);
            found->second.lru_position = verification_cache_->lru.begin();
            return true;
        }
    }

    const bool success = verify_password_uncached(password, encoded_hash);
    if (!success) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock{verification_cache_->mutex};
        const auto found = verification_cache_->entries.find(cache_key);
        if (found != verification_cache_->entries.end()) {
            verification_cache_->lru.erase(found->second.lru_position);
            verification_cache_->lru.push_front(cache_key);
            found->second.lru_position = verification_cache_->lru.begin();
            return true;
        }

        verification_cache_->lru.push_front(cache_key);
        verification_cache_->entries.emplace(
            cache_key,
            VerificationCache::Entry{
                now + verification_cache_->ttl,
                verification_cache_->lru.begin()});

        while (verification_cache_->entries.size() > verification_cache_->max_entries) {
            const auto evict_key = verification_cache_->lru.back();
            verification_cache_->lru.pop_back();
            verification_cache_->entries.erase(evict_key);
        }
    }

    return true;
}

bool PasswordHasher::has_cached_success(
    std::string_view password,
    std::string_view encoded_hash) const {
    if (options_.verification_cache_max_entries == 0 || options_.verification_cache_ttl_ms == 0) {
        return false;
    }
    const auto cache_key = hmac_sha256_hex(verification_cache_->verification_key, encoded_hash, password);
    return use_cached_success(cache_key, std::chrono::steady_clock::now());
}

bool PasswordHasher::use_cached_success(
    const std::string& cache_key,
    std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock{verification_cache_->mutex};
    const auto found = verification_cache_->entries.find(cache_key);
    if (found == verification_cache_->entries.end()) {
        return false;
    }
    if (found->second.expires_at <= now) {
        verification_cache_->lru.erase(found->second.lru_position);
        verification_cache_->entries.erase(found);
        return false;
    }
    verification_cache_->lru.erase(found->second.lru_position);
    verification_cache_->lru.push_front(cache_key);
    found->second.lru_position = verification_cache_->lru.begin();
    return true;
}

bool PasswordHasher::verify_password_uncached(std::string_view password, std::string_view encoded_hash) const {
    if (const auto argon2 = parse_argon2_hash_record(encoded_hash)) {
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30200000L && \
    defined(OSSL_KDF_PARAM_ARGON2_MEMCOST) && defined(OSSL_KDF_PARAM_ARGON2_LANES) && \
    defined(OSSL_KDF_PARAM_THREADS)
        const auto derived = derive_argon2id(
            password,
            argon2->salt,
            argon2->memory_cost_kib,
            argon2->time_cost,
            argon2->parallelism,
            argon2->hash.size());
        return derived.size() == argon2->hash.size() &&
            CRYPTO_memcmp(derived.data(), argon2->hash.data(), derived.size()) == 0;
#else
        return false;
#endif
    }

    if (const auto pbkdf2 = parse_pbkdf2_hash_record(encoded_hash)) {
        // iterations=1 只允许在显式基准模式中验证，避免测试账号被正常/生产服务接受。
        if (pbkdf2->iterations < kPbkdf2DefaultIterations && !options_.benchmark_fast_hash) {
            return false;
        }
        const auto derived = derive_pbkdf2_sha256(
            password,
            pbkdf2->salt,
            pbkdf2->iterations,
            pbkdf2->hash.size());
        return derived.size() == pbkdf2->hash.size() &&
            CRYPTO_memcmp(derived.data(), pbkdf2->hash.data(), derived.size()) == 0;
    }

    return false;
}

std::string PasswordHasher::scheme_name() const {
    return options_.benchmark_fast_hash ? "pbkdf2-sha256" : default_scheme_name();
}

std::string PasswordHasher::default_scheme_name() {
    return kHasOpenSslArgon2 ? "argon2id" : "pbkdf2-sha256";
}

bool PasswordHasher::is_supported_scheme(std::string_view scheme_name) {
    return scheme_name == "argon2id" || scheme_name == "pbkdf2-sha256";
}

}  // namespace mf::auth 命名空间
