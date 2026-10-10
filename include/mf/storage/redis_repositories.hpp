#pragma once

// 文件职责：
// 定义基于 Boost.Redis 的 SessionRepository、聊天最近窗口缓存、聊天状态服务实现。
// 调用流程：
// 仓储工厂在 mysql_redis 模式下构造 RedisSessionRepository ->
// TokenService 调用 save()/find_by_token() ->
// Redis 用作会话存储、TTL 过期控制、聊天最近窗口缓存，以及在线状态/频道成员状态。

#include "mf/chat/state.hpp"
#include "mf/core/logger.hpp"
#include "mf/core/metrics.hpp"
#include "mf/game/models.hpp"
#include "mf/game/repositories.hpp"
#include "mf/storage/options.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mf::storage {

void configure_redis_observability(
    std::shared_ptr<mf::core::Logger> logger,
    std::shared_ptr<mf::core::MetricsRegistry> metrics,
    std::int64_t slow_operation_threshold_ms = 100);

struct AnnouncementCacheDebugSnapshot {
    // AnnouncementCacheDebugSnapshot 用来描述 Redis 公告最近窗口当前长什么样。
    // 调试页或 HTTP 接口可以直接把这个结构转成 JSON，快速确认是否命中缓存、key 是什么、窗口里有哪些公告。
    std::string channel_kind;
    std::string scope_id;
    std::string recent_key;
    std::string warm_key;
    bool warm = false;
    std::vector<mf::game::AnnouncementEntry> items;
};

class SessionRevocationSubscriber {
public:
    virtual ~SessionRevocationSubscriber() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};

std::shared_ptr<mf::game::SessionRepository> make_redis_session_repository(const RedisOptions& options);
std::shared_ptr<SessionRevocationSubscriber> make_redis_session_revocation_subscriber(
    const RedisOptions& options,
    std::function<void(std::string player_id, std::optional<std::string> except_session_id)> on_player_revoked);
std::shared_ptr<mf::game::AuthSecurityRepository> make_redis_auth_security_repository(const RedisOptions& options);
std::shared_ptr<mf::game::ChatRepository> make_redis_cached_chat_repository(
    const RedisOptions& options,
    std::shared_ptr<mf::game::ChatRepository> primary,
    std::size_t window_limit = 100);
std::shared_ptr<mf::game::AnnouncementRepository> make_redis_cached_announcement_repository(
    const RedisOptions& options,
    std::shared_ptr<mf::game::AnnouncementRepository> primary,
    std::size_t window_limit = 20);
std::shared_ptr<mf::chat::ChatStateService> make_redis_chat_state_service(const RedisOptions& options);
std::optional<AnnouncementCacheDebugSnapshot> inspect_announcement_cache(
    const RedisOptions& options,
    mf::game::ChatChannelKind kind,
    std::string scope_id,
    std::size_t limit = 20);

}  // namespace mf::storage 命名空间
