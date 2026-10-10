#pragma once

// 文件职责：
// 定义 token/session 的签发与校验服务。
// 调用流程：
// 登录接口调用 issue() 生成 Session -> SessionRepository 持久化会话 ->
// HTTP 或 WebSocket 收到 bearer token 后调用 authenticate_bearer() 完成鉴权。

#include "mf/game/repositories.hpp"

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mf::auth {

class TokenService {
public:
    explicit TokenService(std::shared_ptr<mf::game::SessionRepository> sessions);

    mf::game::Session issue(
        const std::string& account_id,
        const std::string& player_id,
        const std::string& nickname,
        const mf::game::SessionIssueOptions& options = {},
        std::chrono::minutes ttl = std::chrono::minutes{180});

    std::optional<mf::game::Session> authenticate_bearer(const std::string& authorization_header) const;
    std::optional<mf::game::Session> refresh(
        const std::string& refresh_token,
        std::chrono::minutes ttl = std::chrono::minutes{180});
    // 异步刷新用于 Redis 会话仓储和异步 HTTP 路由，不阻塞业务线程。
    boost::asio::awaitable<std::optional<mf::game::Session>> refresh_async(
        std::string refresh_token,
        std::chrono::minutes ttl = std::chrono::minutes{180});
    bool revoke_bearer(const std::string& authorization_header);
    bool revoke_token(const std::string& token);
    bool revoke_session_id(const std::string& session_id);
    std::size_t revoke_all_for_player(
        const std::string& player_id,
        const std::optional<std::string>& except_session_id = std::nullopt);
    void invalidate_player_sessions(
        const std::string& player_id,
        const std::optional<std::string>& except_session_id = std::nullopt);
    std::vector<mf::game::Session> list_sessions(const std::string& player_id) const;

private:
    struct CachedSession {
        mf::game::Session session;
        std::chrono::steady_clock::time_point cached_at;
    };

    void cache_session(const std::string& token, mf::game::Session session) const;
    void invalidate_token(const std::string& token);
    void invalidate_session_id(const std::string& session_id);
    void invalidate_player(
        const std::string& player_id,
        const std::optional<std::string>& except_session_id);
    // 调用方必须已经持有 cache_mutex_ 的独占锁。
    void erase_cached_token_unlocked(const std::string& token) const;
    mf::game::Session build_session(
        const std::string& account_id,
        const std::string& player_id,
        const std::string& nickname,
        const mf::game::SessionIssueOptions& options,
        std::chrono::minutes ttl) const;
    boost::asio::awaitable<mf::game::Session> issue_async(
        const std::string& account_id,
        const std::string& player_id,
        const std::string& nickname,
        const mf::game::SessionIssueOptions& options,
        std::chrono::minutes ttl);

    std::shared_ptr<mf::game::SessionRepository> sessions_;
    mutable std::shared_mutex cache_mutex_;
    mutable std::unordered_map<std::string, CachedSession> token_cache_;
    // 二级索引避免 refresh/踢下线时扫描整个 token_cache_。
    mutable std::unordered_map<std::string, std::string> token_by_session_id_;
    mutable std::unordered_map<std::string, std::unordered_set<std::string>> tokens_by_player_id_;
    // 过期缓存不需要在每次登录时全量扫描，按固定间隔做摊销清理。
    mutable std::chrono::steady_clock::time_point last_cache_cleanup_{};
};

}  // namespace mf::auth 命名空间
