#include "mf/auth/token_service.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现 token 生成、Session 签发以及 bearer token 校验。
// 调用流程：
// 登录接口调用 issue() -> 生成 token 并写入 SessionRepository ->
// 之后 HTTP/WebSocket 请求带上 token -> authenticate_bearer() 解析并校验有效期。

#include <array>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

#include <openssl/rand.h>

namespace mf::auth {
namespace {

constexpr auto k_token_cache_ttl = std::chrono::seconds{5};
constexpr auto k_token_cache_cleanup_interval = std::chrono::seconds{1};
constexpr std::size_t k_token_cache_max_entries = 20000;

template <std::size_t ByteCount>
std::string generate_random_hex() {
    // 直接使用 OpenSSL CSPRNG，避免每个 token 都重新构造 random_device 和
    // mt19937_64；同时保证 token 仍然是密码学安全的随机值。
    std::array<unsigned char, ByteCount> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed while generating session token");
    }

    constexpr char hex_digits[] = "0123456789abcdef";
    std::string output;
    output.resize(bytes.size() * 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        output[index * 2] = hex_digits[bytes[index] >> 4];
        output[index * 2 + 1] = hex_digits[bytes[index] & 0x0f];
    }
    return output;
}

std::string generate_session_id() {
    // 128 位随机 session_id 只负责定位 Redis 主记录，不单独具备登录能力。
    return generate_random_hex<16>();
}

std::string generate_bound_token(std::string_view session_id) {
    // token = 可定位的 session_id + 256 位随机秘密。
    // 即使 session_id 被会话管理接口展示，攻击者仍无法猜出后半段秘密。
    std::string token;
    token.reserve(session_id.size() + 1 + 64);
    token.append(session_id);
    token.push_back('.');
    token.append(generate_random_hex<32>());
    return token;
}

std::optional<std::string> extract_bearer_token(const std::string& header) {
    constexpr std::string_view prefix = "Bearer ";
    if (!header.starts_with(prefix)) {
        return std::nullopt;
    }
    return header.substr(prefix.size());
}

}  // 匿名命名空间

TokenService::TokenService(std::shared_ptr<mf::game::SessionRepository> sessions)
    : sessions_(std::move(sessions)) {}

mf::game::Session TokenService::issue(
    const std::string& account_id,
    const std::string& player_id,
    const std::string& nickname,
    const mf::game::SessionIssueOptions& options,
    std::chrono::minutes ttl) {
    // TokenService 不关心 HTTP 细节，
    // 它只负责签发和校验 Session，这样同一套鉴权逻辑才能复用于不同传输层。
    auto session = build_session(account_id, player_id, nickname, options, ttl);
    if (options.single_session) {
        sessions_->save_single_session(session);
        // Redis 已经原子撤销旧会话；这里同步清掉本进程的 token 缓存，避免旧 token 继续命中本地缓存。
        invalidate_player(player_id, session.session_id);
    } else {
        sessions_->save(session);
    }
    cache_session(session.token, session);
    return session;
}

mf::game::Session TokenService::build_session(
    const std::string& account_id,
    const std::string& player_id,
    const std::string& nickname,
    const mf::game::SessionIssueOptions& options,
    std::chrono::minutes ttl) const {
    const auto now = std::chrono::system_clock::now();
    auto session_id = generate_session_id();
    auto token = generate_bound_token(session_id);
    auto refresh_token = generate_bound_token(session_id);
    return mf::game::Session{
        .session_id = std::move(session_id),
        .token = std::move(token),
        .refresh_token = std::move(refresh_token),
        .account_id = account_id,
        .player_id = player_id,
        .nickname = nickname,
        .client_ip = options.client_ip,
        .user_agent = options.user_agent,
        .issued_at = now,
        .expires_at = now + ttl,
    };
}

boost::asio::awaitable<mf::game::Session> TokenService::issue_async(
    const std::string& account_id,
    const std::string& player_id,
    const std::string& nickname,
    const mf::game::SessionIssueOptions& options,
    std::chrono::minutes ttl) {
    auto session = build_session(account_id, player_id, nickname, options, ttl);
    if (options.single_session) {
        // refresh 当前固定使用多端会话语义。这里保留完整语义，
        // 未来需要异步 single_session 时再补 save_single_session_async。
        sessions_->save_single_session(session);
        invalidate_player(player_id, session.session_id);
    } else {
        co_await sessions_->save_async(session);
    }
    cache_session(session.token, session);
    co_return session;
}

std::optional<mf::game::Session> TokenService::authenticate_bearer(
    const std::string& authorization_header, bool use_cache) const {
    MF_PROFILE_SCOPE("Auth authenticate bearer");
    const auto token = extract_bearer_token(authorization_header);
    if (!token) {
        return std::nullopt;
    }

    if (use_cache) {
        const auto cache_now = std::chrono::steady_clock::now();
        std::shared_lock lock{cache_mutex_};
        if (const auto it = token_cache_.find(*token); it != token_cache_.end()) {
            if (cache_now - it->second.cached_at < k_token_cache_ttl) {
                const auto session = it->second.session;
                if (session.expires_at >= std::chrono::system_clock::now()) {
                    return session;
                }
            }
        }
    }

    // 过期校验放在这里而不是散落在各个 handler 里，
    // 这样 HTTP、WebSocket 甚至未来 RPC 层都能共享同一套鉴权语义。
    const auto session = sessions_->find_by_token(*token);
    if (!session || session->expires_at < std::chrono::system_clock::now()) {
        return std::nullopt;
    }
    if (use_cache) {
        cache_session(*token, *session);
    }
    return session;
}

std::optional<mf::game::Session> TokenService::refresh(const std::string& refresh_token, std::chrono::minutes ttl) {
    // 仓储原子消费旧 refresh token，防止并发重放，并把 Redis 往返从
    // “查询两次 + 撤销两次 + 保存一次”缩减为“消费一次 + 保存一次”。
    const auto session = sessions_->consume_refresh_token(refresh_token);
    if (!session || session->expires_at < std::chrono::system_clock::now()) {
        return std::nullopt;
    }

    invalidate_session_id(session->session_id);
    return issue(
        session->account_id,
        session->player_id,
        session->nickname,
        mf::game::SessionIssueOptions{
            .single_session = false,
            .client_ip = session->client_ip,
            .user_agent = session->user_agent,
        },
        ttl);
}

boost::asio::awaitable<std::optional<mf::game::Session>> TokenService::refresh_async(
    std::string refresh_token,
    std::chrono::minutes ttl) {
    // 旧 refresh token 的消费和新 Session 的保存都通过异步仓储入口完成。
    const auto session = co_await sessions_->consume_refresh_token_async(std::move(refresh_token));
    if (!session || session->expires_at < std::chrono::system_clock::now()) {
        co_return std::nullopt;
    }

    invalidate_session_id(session->session_id);
    co_return co_await issue_async(
        session->account_id,
        session->player_id,
        session->nickname,
        mf::game::SessionIssueOptions{
            .single_session = false,
            .client_ip = session->client_ip,
            .user_agent = session->user_agent,
        },
        ttl);
}

bool TokenService::revoke_bearer(const std::string& authorization_header) {
    const auto token = extract_bearer_token(authorization_header);
    if (!token) {
        return false;
    }
    return revoke_token(*token);
}

bool TokenService::revoke_token(const std::string& token) {
    const auto revoked = sessions_->revoke_by_token(token);
    invalidate_token(token);
    return revoked;
}

bool TokenService::revoke_session_id(const std::string& session_id) {
    const auto revoked = sessions_->revoke_by_session_id(session_id);
    invalidate_session_id(session_id);
    return revoked;
}

std::size_t TokenService::revoke_all_for_player(
    const std::string& player_id,
    const std::optional<std::string>& except_session_id) {
    const auto revoked = sessions_->revoke_all_for_player(player_id, except_session_id);
    invalidate_player(player_id, except_session_id);
    return revoked;
}

void TokenService::invalidate_player_sessions(
    const std::string& player_id,
    const std::optional<std::string>& except_session_id) {
    invalidate_player(player_id, except_session_id);
}

std::vector<mf::game::Session> TokenService::list_sessions(const std::string& player_id) const {
    return sessions_->list_by_player_id(player_id);
}

void TokenService::cache_session(const std::string& token, mf::game::Session session) const {
    std::unique_lock lock{cache_mutex_};
    const auto now = std::chrono::steady_clock::now();

    // 不要每次登录都扫描整个 unordered_map。登录成功会频繁进入这里，
    // 而缓存查找本身已经会检查 TTL；定期清理即可避免过期项长期占位，
    // 同时把热路径从“每次 O(n)”降为摊销 O(1)。
    if (now - last_cache_cleanup_ >= k_token_cache_cleanup_interval) {
        std::vector<std::string> expired_tokens;
        expired_tokens.reserve(token_cache_.size() / 4);
        for (const auto& [cached_token, cached] : token_cache_) {
            if (now - cached.cached_at >= k_token_cache_ttl) {
                expired_tokens.push_back(cached_token);
            }
        }
        for (const auto& expired_token : expired_tokens) {
            erase_cached_token_unlocked(expired_token);
        }
        last_cache_cleanup_ = now;
    }
    if (token_cache_.size() >= k_token_cache_max_entries) {
        const auto oldest_candidate = token_cache_.begin()->first;
        erase_cached_token_unlocked(oldest_candidate);
    }
    if (token_cache_.contains(token)) {
        erase_cached_token_unlocked(token);
    }
    token_by_session_id_[session.session_id] = token;
    tokens_by_player_id_[session.player_id].insert(token);
    token_cache_[token] = CachedSession{std::move(session), now};
}

void TokenService::invalidate_token(const std::string& token) {
    std::unique_lock lock{cache_mutex_};
    erase_cached_token_unlocked(token);
}

void TokenService::invalidate_session_id(const std::string& session_id) {
    std::unique_lock lock{cache_mutex_};
    if (const auto it = token_by_session_id_.find(session_id); it != token_by_session_id_.end()) {
        const auto token = it->second;
        erase_cached_token_unlocked(token);
    }
}

void TokenService::invalidate_player(
    const std::string& player_id,
    const std::optional<std::string>& except_session_id) {
    std::unique_lock lock{cache_mutex_};
    const auto player_it = tokens_by_player_id_.find(player_id);
    if (player_it == tokens_by_player_id_.end()) {
        return;
    }

    const std::vector<std::string> tokens(player_it->second.begin(), player_it->second.end());
    for (const auto& token : tokens) {
        const auto cached_it = token_cache_.find(token);
        if (cached_it == token_cache_.end()) {
            continue;
        }
        if (except_session_id && cached_it->second.session.session_id == *except_session_id) {
            continue;
        }
        erase_cached_token_unlocked(token);
    }
}

void TokenService::erase_cached_token_unlocked(const std::string& token) const {
    const auto cached_it = token_cache_.find(token);
    if (cached_it == token_cache_.end()) {
        return;
    }

    const auto session_id = cached_it->second.session.session_id;
    const auto player_id = cached_it->second.session.player_id;
    token_by_session_id_.erase(session_id);
    if (const auto player_it = tokens_by_player_id_.find(player_id);
        player_it != tokens_by_player_id_.end()) {
        player_it->second.erase(token);
        if (player_it->second.empty()) {
            tokens_by_player_id_.erase(player_it);
        }
    }
    token_cache_.erase(cached_it);
}

}  // namespace mf::auth 命名空间
