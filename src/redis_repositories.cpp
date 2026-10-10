#include "mf/storage/redis_repositories.hpp"

// 文件职责：
// 实现基于 Boost.Redis 的 SessionRepository，以及 ChatRepository 的最近窗口缓存包装。
// 调用流程：
// TokenService::issue() 调用 save() -> Session 被写入 Redis 并设置 TTL ->
// ChatService::publish() 先把消息落到 MySQL 主仓储 -> 再由 Redis 包装仓储刷新最近窗口缓存 ->
// history() 先读 Redis 最近窗口，命中不足时再回源 MySQL 并回填缓存。

#include "mf/chat/channels.hpp"
#include "mf/game/json.hpp"
#include "mf/core/profiling.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_to.hpp>
#include <boost/redis/config.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/ignore.hpp>
#include <boost/redis/push_parser.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mf::storage {
namespace asio = boost::asio;
namespace redis = boost::redis;

namespace {

struct RedisObservabilityState {
    std::shared_ptr<mf::core::Logger> logger;
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    std::int64_t slow_operation_threshold_ms = 100;
};

std::mutex g_redis_observability_mutex;
RedisObservabilityState g_redis_observability;

void observe_redis_phase(std::string_view phase, double duration_ms) {
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    std::shared_ptr<mf::core::Logger> logger;
    std::int64_t slow_operation_threshold_ms = 100;
    {
        std::scoped_lock lock{g_redis_observability_mutex};
        metrics = g_redis_observability.metrics;
        logger = g_redis_observability.logger;
        slow_operation_threshold_ms = g_redis_observability.slow_operation_threshold_ms;
    }
    if (metrics) {
        metrics->observe_histogram_ms(
            "mf_redis_phase_duration_ms", duration_ms, {{"phase", std::string{phase}}});
        if (duration_ms >= slow_operation_threshold_ms) {
            metrics->increment_counter(
                "mf_alert_events_total", 1.0, {{"category", "slow_redis_operation"}});
        }
    }
    if (logger && duration_ms >= slow_operation_threshold_ms) {
        logger->log(
            mf::core::LogLevel::warn,
            "slow_redis_operation phase=" + std::string{phase} + " duration_ms=" + std::to_string(duration_ms));
    }
}

// RedisExecutor 在后文定义，但构造/执行时会先更新连接池 gauge。
void set_redis_gauge(std::string_view name, double value);
void increment_redis_counter(std::string_view name, std::string_view status);

}  // namespace

void configure_redis_observability(
    std::shared_ptr<mf::core::Logger> logger,
    std::shared_ptr<mf::core::MetricsRegistry> metrics,
    std::int64_t slow_operation_threshold_ms) {
    std::scoped_lock lock{g_redis_observability_mutex};
    g_redis_observability.logger = std::move(logger);
    g_redis_observability.metrics = std::move(metrics);
    g_redis_observability.slow_operation_threshold_ms = std::max<std::int64_t>(1, slow_operation_threshold_ms);
}

namespace {

// Redis 里的计数字段理论上都应该是整数。
// 但线上调试、手工改 key 或旧版本 bug 可能留下脏值；这里统一兜底，避免 HTTP 接口因为 std::stoll 抛异常而返回非 JSON。
std::int64_t parse_redis_i64_or_zero(std::string_view value) {
    try {
        std::size_t parsed = 0;
        const auto number = std::stoll(std::string(value), &parsed);
        return parsed == value.size() ? number : 0;
    } catch (...) {
        return 0;
    }
}

redis::config make_redis_config(const RedisOptions& options) {
    redis::config config;
    if (!options.sentinel_master_name.empty()) {
        for (const auto& sentinel : options.sentinel_addresses) {
            config.sentinel.addresses.push_back(
                redis::address{sentinel.host, std::to_string(sentinel.port)});
        }
        config.sentinel.master_name = options.sentinel_master_name;
        config.sentinel.server_role = redis::role::master;
    } else {
        config.addr.host = options.host;
        config.addr.port = std::to_string(options.port);
    }

    if (!options.password.empty()) {
        config.password = options.password;
    }
    if (options.database_index != 0) {
        config.database_index = options.database_index;
    }
    return config;
}

class SyncRedisConnection {
public:
    explicit SyncRedisConnection(const RedisOptions& options)
        : options_(options),
          ioc_(1),
          connection_(std::make_shared<redis::connection>(ioc_)) {}

    ~SyncRedisConnection() {
        stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void start() {
        // 同一连接可能被多个 HTTP 协程同时首次命中，必须保证只创建一个 Redis I/O 线程。
        std::call_once(start_once_, [this] {
            started_.store(true, std::memory_order_release);
            const auto config = make_redis_config(options_);
            thread_ = std::thread([this, config]() mutable {
                connection_->async_run(config, asio::detached);
                ioc_.run();
            });
        });
    }

    void stop() {
        if (!started_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        asio::dispatch(ioc_, [connection = connection_]() {
            connection->cancel();
        });
    }

    template <class Response>
    void exec(const redis::request& request, Response& response) const {
        MF_PROFILE_SCOPE("Redis sync exec");
        const auto wait_started_at = std::chrono::steady_clock::now();
        double mutex_wait_ms = 0.0;
        double command_ms = 0.0;
        {
            std::scoped_lock lock{mutex_};
            mutex_wait_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_started_at)
                    .count();
            const_cast<SyncRedisConnection*>(this)->start();
            const auto command_started_at = std::chrono::steady_clock::now();
            asio::dispatch(connection_->get_executor(), asio::deferred([this, &request, &response]() {
                return connection_->async_exec(request, response, asio::deferred);
            }))(asio::use_future)
                .get();
            command_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - command_started_at)
                    .count();
        }
        observe_redis_phase("mutex_wait", mutex_wait_ms);
        observe_redis_phase("command", command_ms);
    }

    // 协程版本直接等待 Boost.Redis 的完成事件，不创建 future，也不调用 get()。
    // request 按值接收，保证协程挂起期间请求对象仍然有效。
    template <class Response>
    asio::awaitable<void> exec_async(redis::request request, Response& response) const {
        MF_PROFILE_SCOPE("Redis async exec");
        const_cast<SyncRedisConnection*>(this)->start();
        const auto command_started_at = std::chrono::steady_clock::now();
        // Boost.Redis 连接对象归自己的 io_context 管理。通过 co_spawn 把操作发起固定到该 executor，
        // 完成后再恢复调用方协程，避免多个 HTTP 线程直接并发触碰连接内部状态。
        co_await asio::co_spawn(
            connection_->get_executor(),
            [connection = connection_, request = std::move(request), &response]() mutable
                -> asio::awaitable<void> {
                co_await connection->async_exec(request, response);
            },
            asio::use_awaitable);
        observe_redis_phase(
            "async_command",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - command_started_at)
                .count());
    }

private:
    RedisOptions options_;
    asio::io_context ioc_;
    std::shared_ptr<redis::connection> connection_;
    std::thread thread_;
    std::once_flag start_once_;
    std::atomic<bool> started_{false};
    mutable std::mutex mutex_;
};

// RedisExecutor 是一个轻量复用层：
// 1. 内部维护多个同步 Redis 连接，降低单连接串行执行带来的锁竞争。
// 2. 每次请求按轮询挑一个连接执行，尽量把不同仓储的 Redis 访问打散到多个连接上。
// 3. 所有 Redis 仓储共享同一套 executor，避免“每个仓储自己起线程和连接”。
class RedisExecutor {
public:
    explicit RedisExecutor(RedisOptions options) {
        const auto pool_size = std::max<std::size_t>(1, options.pool_size);
        pool_size_ = pool_size;
        connections_.reserve(pool_size);
        for (std::size_t index = 0; index < pool_size; ++index) {
            auto connection = std::make_shared<SyncRedisConnection>(options);
            // 连接池构造时就启动连接，避免第一批业务请求承担线程创建和 Redis 握手延迟。
            connection->start();
            connections_.push_back(std::move(connection));
        }
        set_redis_gauge("mf_redis_pool_size", static_cast<double>(pool_size_));
        set_redis_gauge("mf_redis_pool_in_flight", 0.0);
    }

    template <class Response>
    void exec(const redis::request& request, Response& response) const {
        enter_exec();
        try {
            const auto index = next_index_.fetch_add(1, std::memory_order_relaxed) % connections_.size();
            connections_.at(index)->exec(request, response);
            increment_redis_counter("mf_redis_operations_total", "ok");
        } catch (...) {
            increment_redis_counter("mf_redis_operations_total", "error");
            leave_exec();
            throw;
        }
        leave_exec();
    }

    template <class Response>
    asio::awaitable<void> exec_async(redis::request request, Response& response) const {
        enter_exec();
        try {
            const auto index = next_index_.fetch_add(1, std::memory_order_relaxed) % connections_.size();
            co_await connections_.at(index)->exec_async(std::move(request), response);
            increment_redis_counter("mf_redis_operations_total", "ok");
        } catch (...) {
            increment_redis_counter("mf_redis_operations_total", "error");
            leave_exec();
            throw;
        }
        leave_exec();
    }

private:
    void enter_exec() const {
        const auto count = in_flight_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        set_redis_gauge("mf_redis_pool_in_flight", static_cast<double>(count));
    }

    void leave_exec() const {
        auto count = in_flight_count_.load(std::memory_order_relaxed);
        while (count > 0 &&
               !in_flight_count_.compare_exchange_weak(
                   count, count - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
        set_redis_gauge("mf_redis_pool_in_flight", static_cast<double>(count > 0 ? count - 1 : 0));
    }

    std::vector<std::shared_ptr<SyncRedisConnection>> connections_;
    mutable std::atomic<std::size_t> next_index_{0};
    std::size_t pool_size_ = 0;
    mutable std::atomic<std::size_t> in_flight_count_{0};
};

std::string redis_executor_cache_key(const RedisOptions& options) {
    std::ostringstream stream;
    stream << options.host << ':' << options.port << '|'
           << options.password << '|' << options.database_index << '|'
           << options.pool_size << '|'
           << options.sentinel_master_name << '|';
    for (const auto& sentinel : options.sentinel_addresses) {
        stream << sentinel.host << ':' << sentinel.port << ',';
    }
    return stream.str();
}

std::shared_ptr<RedisExecutor> shared_redis_executor(const RedisOptions& options) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<RedisExecutor>> cache;

    const auto key = redis_executor_cache_key(options);
    std::scoped_lock lock{mutex};
    if (const auto existing = cache[key].lock()) {
        return existing;
    }

    auto created = std::make_shared<RedisExecutor>(options);
    cache[key] = created;
    return created;
}

std::string session_key(const std::string& token) {
    return "session:" + token;
}

std::string session_id_key(const std::string& session_id) {
    return "session:id:" + session_id;
}

std::string session_token_lookup_key(const std::string& token) {
    return "session:token:" + token;
}

std::string session_refresh_lookup_key(const std::string& refresh_token) {
    return "session:refresh:" + refresh_token;
}

std::string player_sessions_key(const std::string& player_id) {
    return "session:player:" + player_id;
}

std::optional<std::string_view> bound_session_id(std::string_view token) {
    // 新 token 格式固定为 32 位十六进制 session_id + '.' + 随机秘密。
    // 严格校验前缀，避免把任意客户端输入直接拼进 Redis key。
    constexpr std::size_t kSessionIdLength = 32;
    if (token.size() <= kSessionIdLength + 1 || token[kSessionIdLength] != '.') {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < kSessionIdLength; ++index) {
        const auto ch = token[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return std::nullopt;
        }
    }
    return token.substr(0, kSessionIdLength);
}

bool uses_bound_session_tokens(const mf::game::Session& session) {
    const auto access_session_id = bound_session_id(session.token);
    const auto refresh_session_id = bound_session_id(session.refresh_token);
    return access_session_id && refresh_session_id &&
           *access_session_id == session.session_id &&
           *refresh_session_id == session.session_id;
}

constexpr std::string_view kSessionRevocationChannel = "session:revocation";

constexpr std::string_view kSaveSingleSessionScript = R"LUA(
local player_key = KEYS[1]
local session_id_key = KEYS[2]
local token_lookup_key = KEYS[3]
local refresh_lookup_key = KEYS[4]
local payload = ARGV[1]
local ttl = ARGV[2]
local session_id = ARGV[3]
local player_id = ARGV[4]

-- 旧 token/refresh_token 的反向索引即使暂时保留也不能通过鉴权：
-- 它们最终还要读取 session:id:<id>，而旧实体已被删除。索引会按原 TTL 自动过期。
-- 直接删除玩家会话集合和旧实体，避免 SMEMBERS + GET + JSON decode 的循环锁住 Redis。
local members = redis.call('SMEMBERS', player_key)
for i = 1, #members do
    redis.call('DEL', 'session:id:' .. members[i])
end
redis.call('DEL', player_key)

redis.call('SET', session_id_key, payload, 'EX', ttl)
redis.call('SET', token_lookup_key, session_id, 'EX', ttl)
redis.call('SET', refresh_lookup_key, session_id, 'EX', ttl)
redis.call('SADD', player_key, session_id)
redis.call('EXPIRE', player_key, ttl)
redis.call('PUBLISH', 'session:revocation', cjson.encode({player_id = player_id, session_id = session_id}))
return 1
)LUA";

constexpr std::string_view kSaveSingleBoundSessionScript = R"LUA(
local player_key = KEYS[1]
local session_key = KEYS[2]
local payload = ARGV[1]
local ttl = ARGV[2]
local session_id = ARGV[3]
local player_id = ARGV[4]

local members = redis.call('SMEMBERS', player_key)
for i = 1, #members do
    redis.call('DEL', 'session:id:' .. members[i])
end
redis.call('DEL', player_key)

redis.call('SET', session_key, payload, 'EX', ttl)
redis.call('SADD', player_key, session_id)
redis.call('EXPIRE', player_key, ttl)
redis.call('PUBLISH', 'session:revocation', cjson.encode({player_id = player_id, session_id = session_id}))
return 1
)LUA";

// refresh token 必须只能使用一次。Lua 在 Redis 内原子完成读取和撤销，
// 避免两个并发请求都在“查询成功、删除之前”的窗口拿到同一个旧会话。
constexpr std::string_view kConsumeRefreshTokenScript = R"LUA(
local session_id = redis.call('GET', KEYS[1])
if not session_id then
    return false
end

local session_key = 'session:id:' .. session_id
local payload = redis.call('GET', session_key)
redis.call('DEL', KEYS[1])
if not payload then
    return false
end

local session = cjson.decode(payload)
redis.call('DEL', session_key)
redis.call('DEL', 'session:token:' .. session.token)
redis.call('DEL', 'session:refresh:' .. session.refresh_token)
redis.call('SREM', 'session:player:' .. session.player_id, session_id)
return payload
)LUA";

constexpr std::string_view kConsumeBoundRefreshTokenScript = R"LUA(
local payload = redis.call('GET', KEYS[1])
if not payload then
    return false
end

local session = cjson.decode(payload)
if session.refresh_token ~= ARGV[1] then
    return false
end

redis.call('DEL', KEYS[1])
redis.call('SREM', 'session:player:' .. session.player_id, ARGV[2])
return payload
)LUA";

std::string captcha_key(const std::string& challenge_id) {
    return "auth:captcha:" + challenge_id;
}

std::string login_risk_key(const std::string& login_name, const std::string& client_ip) {
    return "auth:risk:" + login_name + ":" + client_ip;
}

std::string registration_pair_key(const std::string& login_name, const std::string& client_ip) {
    return "auth:register:pair:" + login_name + ":" + client_ip;
}

std::string registration_ip_key(const std::string& client_ip) {
    return "auth:register:ip:" + client_ip;
}

std::string blacklist_key(const std::string& subject_type, const std::string& subject_value) {
    return "auth:blacklist:" + subject_type + ":" + subject_value;
}

std::string blacklist_meta_key(const std::string& entry_id) {
    return "auth:blacklist:meta:" + entry_id;
}

std::string blacklist_index_key() {
    return "auth:blacklist:index";
}

std::string recent_messages_key(const std::string& channel_id) {
    return "chat:recent:" + channel_id;
}

std::string recent_messages_warm_key(const std::string& channel_id) {
    return "chat:recent:warm:" + channel_id;
}

void set_redis_gauge(std::string_view name, double value) {
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    {
        std::scoped_lock lock{g_redis_observability_mutex};
        metrics = g_redis_observability.metrics;
    }
    if (metrics) {
        metrics->set_gauge(std::string{name}, value, {{"pool", "redis"}});
    }
}

void increment_redis_counter(std::string_view name, std::string_view status) {
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    {
        std::scoped_lock lock{g_redis_observability_mutex};
        metrics = g_redis_observability.metrics;
    }
    if (metrics) {
        metrics->increment_counter(
            std::string{name}, 1.0, {{"status", std::string{status}}});
    }
}

std::string latest_message_cache_key(const std::string& channel_id) {
    return "chat:latest:" + channel_id;
}

// 批量回填/更新每个频道的最新消息快照；旧消息不得覆盖并发写入的新消息。
constexpr std::string_view kWriteLatestChatMessagesScript = R"LUA(
local count = #KEYS
local ttl = ARGV[count * 3 + 1]
for i = 1, count do
    local payload = ARGV[(i - 1) * 3 + 1]
    local sent_at = tonumber(ARGV[(i - 1) * 3 + 2]) or 0
    local message_id = ARGV[(i - 1) * 3 + 3]
    local current = redis.call('GET', KEYS[i])
    if payload == '' then
        if not current then
            redis.call('SET', KEYS[i], '', 'EX', ttl)
        end
    else
        local should_write = not current or current == ''
        if current and current ~= '' then
            local ok, decoded = pcall(cjson.decode, current)
            if not ok or type(decoded) ~= 'table' then
                should_write = true
            else
                local current_sent_at = tonumber(decoded.sent_at) or 0
                local current_id = decoded.message_id or ''
                should_write = sent_at > current_sent_at or
                    (sent_at == current_sent_at and message_id >= current_id)
            end
        end
        if should_write then
            redis.call('SET', KEYS[i], payload, 'EX', ttl)
        end
    end
end
return count
)LUA";

// 原子更新好友私聊状态：始终增加未读数，只有接收方当前离线时才保存离线消息。
// 把 SISMEMBER、HINCRBY、RPUSH、LTRIM 放进一次 EVAL，避免每条消息多次借用连接和往返 Redis。
constexpr std::string_view kRecordFriendMessageStateScript = R"LUA(
local is_online = redis.call('SISMEMBER', KEYS[3], ARGV[1])
redis.call('HINCRBY', KEYS[1], ARGV[2], 1)
if is_online == 0 then
    redis.call('RPUSH', KEYS[2], ARGV[3])
    redis.call('LTRIM', KEYS[2], '-200', '-1')
end
return is_online
)LUA";

std::string recent_announcements_key(const std::string& kind, const std::string& scope) {
    return "announcement:recent:" + kind + ":" + scope;
}

std::string recent_announcements_warm_key(const std::string& kind, const std::string& scope) {
    return "announcement:recent:warm:" + kind + ":" + scope;
}

std::string online_count_key(const std::string& player_id) {
    return "chat:online:count:" + player_id;
}

std::string online_players_key() {
    return "chat:online:players";
}

std::string channel_members_key(const std::string& channel_id) {
    return "chat:channel:members:" + channel_id;
}

std::string channel_member_count_key(const std::string& channel_id, const std::string& player_id) {
    return "chat:channel:membercount:" + channel_id + ":" + player_id;
}

std::string unread_hash_key(const std::string& player_id) {
    return "chat:unread:" + player_id;
}

std::string offline_messages_key(const std::string& player_id) {
    return "chat:offline:" + player_id;
}

std::string serialize_session(const mf::game::Session& session) {
    const boost::json::object object{
        {"session_id", session.session_id},
        {"token", session.token},
        {"refresh_token", session.refresh_token},
        {"account_id", session.account_id},
        {"player_id", session.player_id},
        {"nickname", session.nickname},
        {"client_ip", session.client_ip},
        {"user_agent", session.user_agent},
        {"issued_at", std::chrono::duration_cast<std::chrono::seconds>(
                          session.issued_at.time_since_epoch())
                          .count()},
        {"expires_at", std::chrono::duration_cast<std::chrono::seconds>(
                           session.expires_at.time_since_epoch())
                           .count()},
    };
    return boost::json::serialize(object);
}

std::optional<mf::game::Session> deserialize_session(const std::optional<std::string>& payload) {
    if (!payload || payload->empty()) {
        return std::nullopt;
    }

    const auto value = boost::json::parse(*payload).as_object();
    return mf::game::Session{
        .session_id = boost::json::value_to<std::string>(value.at("session_id")),
        .token = boost::json::value_to<std::string>(value.at("token")),
        .refresh_token = boost::json::value_to<std::string>(value.at("refresh_token")),
        .account_id = boost::json::value_to<std::string>(value.at("account_id")),
        .player_id = boost::json::value_to<std::string>(value.at("player_id")),
        .nickname = boost::json::value_to<std::string>(value.at("nickname")),
        .client_ip = boost::json::value_to<std::string>(value.at("client_ip")),
        .user_agent = boost::json::value_to<std::string>(value.at("user_agent")),
        .issued_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{value.at("issued_at").as_int64()}},
        .expires_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{value.at("expires_at").as_int64()}},
    };
}

std::string serialize_chat_message(const mf::game::ChatMessage& message) {
    return boost::json::serialize(mf::game::to_json(message));
}

std::optional<mf::game::ChatMessage> deserialize_chat_message(const std::optional<std::string>& payload) {
    if (!payload || payload->empty()) {
        return std::nullopt;
    }

    const auto object = boost::json::parse(*payload).as_object();
    const auto& channel_object = object.at("channel").as_object();

    const auto channel_kind =
        mf::chat::parse_channel_kind(boost::json::value_to<std::string>(channel_object.at("kind")))
            .value_or(mf::game::ChatChannelKind::lobby);
    const auto message_kind =
        mf::chat::parse_message_kind(boost::json::value_to<std::string>(object.at("kind")))
            .value_or(mf::game::ChatMessageKind::text);

    return mf::game::ChatMessage{
        .message_id = boost::json::value_to<std::string>(object.at("message_id")),
        .channel =
            mf::game::ChatChannel{
                .channel_id = boost::json::value_to<std::string>(channel_object.at("channel_id")),
                .kind = channel_kind,
                .scope_id = boost::json::value_to<std::string>(channel_object.at("scope_id")),
                .display_name = boost::json::value_to<std::string>(channel_object.at("display_name")),
            },
        .kind = message_kind,
        .sender_player_id = boost::json::value_to<std::string>(object.at("sender_player_id")),
        .sender_nickname = boost::json::value_to<std::string>(object.at("sender_nickname")),
        .text = boost::json::value_to<std::string>(object.at("text")),
        .recalled = object.if_contains("recalled") ? object.at("recalled").as_bool() : false,
        .recalled_by_player_id =
            object.if_contains("recalled_by_player_id")
                ? boost::json::value_to<std::string>(object.at("recalled_by_player_id"))
                : std::string{},
        .recalled_at =
            (object.if_contains("recalled_at") && !object.at("recalled_at").is_null())
                ? std::optional<std::chrono::system_clock::time_point>{
                      std::chrono::system_clock::time_point{
                          std::chrono::seconds{object.at("recalled_at").as_int64()}}}
                : std::nullopt,
        .sent_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{object.at("sent_at").as_int64()}},
    };
}

std::string serialize_blacklist_entry(const mf::game::BlacklistEntry& entry) {
    return boost::json::serialize(mf::game::to_json(entry));
}

std::string serialize_announcement(const mf::game::AnnouncementEntry& entry) {
    return boost::json::serialize(mf::game::to_json(entry));
}

std::optional<mf::game::AnnouncementEntry> deserialize_announcement(const std::optional<std::string>& payload) {
    if (!payload || payload->empty()) {
        return std::nullopt;
    }
    const auto object = boost::json::parse(*payload).as_object();
    const auto& channel_object = object.at("channel").as_object();
    const auto channel_kind =
        mf::chat::parse_channel_kind(boost::json::value_to<std::string>(channel_object.at("kind")))
            .value_or(mf::game::ChatChannelKind::lobby);
    std::optional<std::chrono::system_clock::time_point> published_at;
    if (const auto* value = object.if_contains("published_at"); value && !value->is_null()) {
        published_at = std::chrono::system_clock::time_point{std::chrono::seconds{value->as_int64()}};
    }
    std::optional<std::chrono::system_clock::time_point> expires_at;
    if (const auto* value = object.if_contains("expires_at"); value && !value->is_null()) {
        expires_at = std::chrono::system_clock::time_point{std::chrono::seconds{value->as_int64()}};
    }
    return mf::game::AnnouncementEntry{
        .announcement_id = boost::json::value_to<std::string>(object.at("announcement_id")),
        .channel =
            mf::game::ChatChannel{
                .channel_id = boost::json::value_to<std::string>(channel_object.at("channel_id")),
                .kind = channel_kind,
                .scope_id = boost::json::value_to<std::string>(channel_object.at("scope_id")),
                .display_name = boost::json::value_to<std::string>(channel_object.at("display_name")),
            },
        .operator_id = boost::json::value_to<std::string>(object.at("operator_id")),
        .title = boost::json::value_to<std::string>(object.at("title")),
        .body = boost::json::value_to<std::string>(object.at("body")),
        .published_at = published_at,
        .expires_at = expires_at,
    };
}

std::optional<mf::game::BlacklistEntry> deserialize_blacklist_entry(const std::optional<std::string>& payload) {
    if (!payload || payload->empty()) {
        return std::nullopt;
    }

    const auto object = boost::json::parse(*payload).as_object();
    std::optional<std::chrono::system_clock::time_point> expires_at;
    if (const auto* value = object.if_contains("expires_at"); value && !value->is_null()) {
        expires_at = std::chrono::system_clock::time_point{std::chrono::seconds{value->as_int64()}};
    }
    return mf::game::BlacklistEntry{
        .entry_id = boost::json::value_to<std::string>(object.at("entry_id")),
        .subject_type = boost::json::value_to<std::string>(object.at("subject_type")),
        .subject_value = boost::json::value_to<std::string>(object.at("subject_value")),
        .reason = boost::json::value_to<std::string>(object.at("reason")),
        .expires_at = expires_at,
    };
}

std::vector<mf::game::ChatMessage> decode_chat_messages(
    const std::vector<std::optional<std::string>>& values,
    bool reverse_order) {
    std::vector<mf::game::ChatMessage> messages;
    messages.reserve(values.size());
    for (const auto& value : values) {
        if (const auto message = deserialize_chat_message(value)) {
            messages.push_back(*message);
        }
    }

    if (reverse_order) {
        std::reverse(messages.begin(), messages.end());
    }
    return messages;
}

std::vector<mf::game::AnnouncementEntry> decode_announcements(
    const std::vector<std::optional<std::string>>& payloads) {
    std::vector<mf::game::AnnouncementEntry> items;
    items.reserve(payloads.size());
    for (const auto& payload : payloads) {
        if (const auto entry = deserialize_announcement(payload)) {
            items.push_back(*entry);
        }
    }
    return items;
}

std::vector<std::string> friend_channel_participants(const mf::game::ChatChannel& channel) {
    if (channel.kind != mf::game::ChatChannelKind::friend_direct) {
        return {};
    }

    std::vector<std::string> players;
    std::stringstream input(channel.scope_id);
    std::string token;
    while (std::getline(input, token, ':')) {
        if (!token.empty()) {
            players.push_back(token);
        }
    }
    return players;
}

std::int64_t unix_seconds(std::chrono::system_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

template <class T>
std::optional<T> result_value_or_none(const boost::system::result<T, redis::adapter::error>& result) {
    if (!result.has_value()) {
        return std::nullopt;
    }
    return result.value();
}

std::optional<std::string> redis_optional_string_or_none(
    const boost::system::result<std::optional<std::string>, redis::adapter::error>& result) {
    if (!result.has_value()) {
        return std::nullopt;
    }
    return result.value();
}

std::string make_random_id(std::string_view prefix) {
    static std::atomic<std::uint64_t> sequence = 1;
    std::ostringstream output;
    output << prefix
           << std::hex
           << std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count()
           << sequence.fetch_add(1, std::memory_order_relaxed);
    return output.str();
}

class RedisSessionRepository final : public mf::game::SessionRepository {
public:
    explicit RedisSessionRepository(RedisOptions options)
        : connection_(shared_redis_executor(options)) {}

    void save(mf::game::Session session) override {
        const auto ttl = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<std::chrono::seconds>(
                session.expires_at - std::chrono::system_clock::now())
                .count());

        redis::request request;
        request.push("SET", session_id_key(session.session_id), serialize_session(session), "EX", std::to_string(ttl));
        if (!uses_bound_session_tokens(session)) {
            // 旧格式 token 没有携带 session_id，继续写反向索引以保持兼容。
            request.push("SET", session_token_lookup_key(session.token), session.session_id, "EX", std::to_string(ttl));
            request.push(
                "SET",
                session_refresh_lookup_key(session.refresh_token),
                session.session_id,
                "EX",
                std::to_string(ttl));
        }
        request.push("SADD", player_sessions_key(session.player_id), session.session_id);
        request.push("EXPIRE", player_sessions_key(session.player_id), std::to_string(ttl));
        // 登录成功只关心写命令是否报错，不读取 SET/SADD/EXPIRE 的返回正文。
        // ignore 仍会传播 Redis 错误，同时避免构造 3 个 "OK" 字符串和 2 个整数响应。
        connection_->exec(request, redis::ignore);
    }

    asio::awaitable<void> save_async(mf::game::Session session) override {
        const auto ttl = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<std::chrono::seconds>(
                session.expires_at - std::chrono::system_clock::now())
                .count());

        redis::request request;
        request.push("SET", session_id_key(session.session_id), serialize_session(session), "EX", std::to_string(ttl));
        if (!uses_bound_session_tokens(session)) {
            request.push("SET", session_token_lookup_key(session.token), session.session_id, "EX", std::to_string(ttl));
            request.push(
                "SET",
                session_refresh_lookup_key(session.refresh_token),
                session.session_id,
                "EX",
                std::to_string(ttl));
        }
        request.push("SADD", player_sessions_key(session.player_id), session.session_id);
        request.push("EXPIRE", player_sessions_key(session.player_id), std::to_string(ttl));
        // 异步登录链路同样忽略成功响应正文，只保留协议错误和 Redis 错误检查。
        co_await connection_->exec_async(std::move(request), redis::ignore);
    }

    void save_single_session(mf::game::Session session) override {
        const auto ttl = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<std::chrono::seconds>(
                session.expires_at - std::chrono::system_clock::now())
                .count());

        redis::request request;
        if (uses_bound_session_tokens(session)) {
            request.push(
                "EVAL",
                std::string{kSaveSingleBoundSessionScript},
                "2",
                player_sessions_key(session.player_id),
                session_id_key(session.session_id),
                serialize_session(session),
                std::to_string(ttl),
                session.session_id,
                session.player_id);
        } else {
            request.push(
                "EVAL",
                std::string{kSaveSingleSessionScript},
                "4",
                player_sessions_key(session.player_id),
                session_id_key(session.session_id),
                session_token_lookup_key(session.token),
                session_refresh_lookup_key(session.refresh_token),
                serialize_session(session),
                std::to_string(ttl),
                session.session_id,
                session.player_id);
        }
        redis::response<std::int64_t> response;
        connection_->exec(request, response);
    }

    std::optional<mf::game::Session> find_by_token(const std::string& token) const override {
        if (const auto session_id = bound_session_id(token)) {
            const auto session = find_by_session_id(std::string{*session_id});
            if (!session || session->token != token) {
                return std::nullopt;
            }
            return session;
        }

        redis::request request;
        request.push("GET", session_token_lookup_key(token));
        redis::response<std::optional<std::string>> response;
        connection_->exec(request, response);
        const auto& session_id_result = std::get<0>(response);
        if (!session_id_result.has_value()) {
            return std::nullopt;
        }

        const auto& session_id = session_id_result.value();
        if (!session_id.has_value()) {
            return std::nullopt;
        }
        return find_by_session_id(*session_id);
    }

    std::optional<mf::game::Session> find_by_refresh_token(const std::string& refresh_token) const override {
        if (const auto session_id = bound_session_id(refresh_token)) {
            const auto session = find_by_session_id(std::string{*session_id});
            if (!session || session->refresh_token != refresh_token) {
                return std::nullopt;
            }
            return session;
        }

        redis::request request;
        request.push("GET", session_refresh_lookup_key(refresh_token));
        redis::response<std::optional<std::string>> response;
        connection_->exec(request, response);
        const auto& session_id_result = std::get<0>(response);
        if (!session_id_result.has_value()) {
            return std::nullopt;
        }

        const auto& session_id = session_id_result.value();
        if (!session_id.has_value()) {
            return std::nullopt;
        }
        return find_by_session_id(*session_id);
    }

    std::optional<mf::game::Session> consume_refresh_token(const std::string& refresh_token) override {
        redis::request request;
        if (const auto session_id = bound_session_id(refresh_token)) {
            request.push(
                "EVAL",
                std::string{kConsumeBoundRefreshTokenScript},
                "1",
                session_id_key(std::string{*session_id}),
                refresh_token,
                std::string{*session_id});
        } else {
            request.push(
                "EVAL",
                std::string{kConsumeRefreshTokenScript},
                "1",
                session_refresh_lookup_key(refresh_token));
        }
        redis::response<std::optional<std::string>> response;
        connection_->exec(request, response);

        const auto& payload_result = std::get<0>(response);
        if (!payload_result.has_value()) {
            return std::nullopt;
        }
        return deserialize_session(redis_optional_string_or_none(payload_result));
    }

    asio::awaitable<std::optional<mf::game::Session>> consume_refresh_token_async(
        std::string refresh_token) override {
        redis::request request;
        if (const auto session_id = bound_session_id(refresh_token)) {
            request.push(
                "EVAL",
                std::string{kConsumeBoundRefreshTokenScript},
                "1",
                session_id_key(std::string{*session_id}),
                refresh_token,
                std::string{*session_id});
        } else {
            request.push(
                "EVAL",
                std::string{kConsumeRefreshTokenScript},
                "1",
                session_refresh_lookup_key(refresh_token));
        }
        redis::response<std::optional<std::string>> response;
        co_await connection_->exec_async(std::move(request), response);

        const auto& payload_result = std::get<0>(response);
        if (!payload_result.has_value()) {
            co_return std::nullopt;
        }
        co_return deserialize_session(redis_optional_string_or_none(payload_result));
    }

    std::vector<mf::game::Session> list_by_player_id(const std::string& player_id) const override {
        redis::request request;
        request.push("SMEMBERS", player_sessions_key(player_id));
        redis::response<std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        std::vector<std::string> session_ids;
        const auto values = result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{});
        for (const auto& value : values) {
            if (value && !value->empty()) {
                session_ids.push_back(*value);
            }
        }

        std::vector<mf::game::Session> sessions;
        sessions.reserve(session_ids.size());
        for (const auto& session_id : session_ids) {
            if (const auto session = find_by_session_id(session_id)) {
                sessions.push_back(*session);
            }
        }
        return sessions;
    }

    bool revoke_by_token(const std::string& token) override {
        const auto session = find_by_token(token);
        if (!session) {
            return false;
        }
        return revoke_session(*session);
    }

    bool revoke_by_session_id(const std::string& session_id) override {
        const auto session = find_by_session_id(session_id);
        if (!session) {
            return false;
        }
        return revoke_session(*session);
    }

    std::size_t revoke_all_for_player(
        const std::string& player_id,
        const std::optional<std::string>& except_session_id) override {
        const auto sessions = list_by_player_id(player_id);
        std::size_t removed = 0;
        for (const auto& session : sessions) {
            if (except_session_id && *except_session_id == session.session_id) {
                continue;
            }
            if (revoke_session(session)) {
                ++removed;
            }
        }
        return removed;
    }

private:
    std::optional<mf::game::Session> find_by_session_id(const std::string& session_id) const {
        redis::request request;
        request.push("GET", session_id_key(session_id));
        redis::response<std::optional<std::string>> response;
        connection_->exec(request, response);

        const auto& session_payload_result = std::get<0>(response);
        if (!session_payload_result.has_value()) {
            return std::nullopt;
        }
        return deserialize_session(redis_optional_string_or_none(session_payload_result));
    }

    bool revoke_session(const mf::game::Session& session) const {
        redis::request request;
        request.push("DEL", session_id_key(session.session_id));
        if (!uses_bound_session_tokens(session)) {
            request.push("DEL", session_token_lookup_key(session.token));
            request.push("DEL", session_refresh_lookup_key(session.refresh_token));
        }
        request.push("SREM", player_sessions_key(session.player_id), session.session_id);
        connection_->exec(request, redis::ignore);
        return true;
    }

    std::shared_ptr<RedisExecutor> connection_;
};

class RedisAuthSecurityRepository final : public mf::game::AuthSecurityRepository {
public:
    explicit RedisAuthSecurityRepository(RedisOptions options)
        : connection_(shared_redis_executor(options)) {}

    mf::game::CaptchaChallenge create_captcha(
        std::string prompt,
        std::string expected_answer,
        std::chrono::system_clock::time_point expires_at) override {
        const auto challenge_id = make_random_id("captcha_");
        const auto ttl = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<std::chrono::seconds>(expires_at - std::chrono::system_clock::now()).count());

        redis::request request;
        request.push("SET", captcha_key(challenge_id), expected_answer, "EX", std::to_string(ttl));
        redis::response<std::string> response;
        connection_->exec(request, response);

        return mf::game::CaptchaChallenge{
            .challenge_id = challenge_id,
            .prompt = std::move(prompt),
            .expires_at = expires_at,
        };
    }

    bool verify_and_consume_captcha(const std::string& challenge_id, const std::string& answer) override {
        redis::request request;
        request.push("GET", captcha_key(challenge_id));
        request.push("DEL", captcha_key(challenge_id));
        redis::response<std::optional<std::string>, std::int64_t> response;
        connection_->exec(request, response);

        // GET 命中不到时会返回 Redis NIL，这里必须按“校验失败”处理，
        // 不能直接对空 optional 调 value()，否则验证码过期或重复提交会抛异常。
        const auto& stored_result = std::get<0>(response);
        if (!stored_result.has_value()) {
            return false;
        }

        const auto stored = redis_optional_string_or_none(stored_result);
        return stored.has_value() && *stored == answer;
    }

    mf::game::LoginRiskState inspect_login_risk(
        const std::string& login_name,
        const std::string& client_ip) const override {
        redis::request request;
        request.push("HGETALL", login_risk_key(login_name, client_ip));
        redis::response<std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        mf::game::LoginRiskState state;
        const auto values = result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{});
        std::unordered_map<std::string, std::string> fields;
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            if (values[i] && values[i + 1]) {
                fields.emplace(*values[i], *values[i + 1]);
            }
        }

        if (const auto it = fields.find("failure_count"); it != fields.end()) {
            state.failure_count = std::stoll(it->second);
            state.captcha_required = state.failure_count >= 3;
        }
        if (const auto it = fields.find("blocked_until"); it != fields.end()) {
            const auto blocked_until = std::stoll(it->second);
            const auto now = unix_seconds(std::chrono::system_clock::now());
            if (blocked_until > now) {
                state.rate_limited = true;
                state.retry_after_seconds = blocked_until - now;
            }
        }
        return state;
    }

    // 一次 pipeline 同时读取 IP 黑名单、登录名黑名单和风控 Hash，
    // 减少登录关键路径上的 Redis 网络往返。
    mf::game::LoginRiskState inspect_login_access(
        const std::string& login_name,
        const std::string& client_ip) const override {
        const auto cache_key = clean_access_cache_key(login_name, client_ip);
        if (has_clean_access_cache(cache_key)) {
            increment_redis_counter("mf_auth_clean_access_cache_total", "hit");
            return {};
        }
        increment_redis_counter("mf_auth_clean_access_cache_total", "miss");

        redis::request request;
        request.push("GET", blacklist_key("ip", client_ip));
        request.push("GET", blacklist_key("login_name", login_name));
        request.push("HGETALL", login_risk_key(login_name, client_ip));
        redis::response<
            std::optional<std::string>,
            std::optional<std::string>,
            std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        const auto has_blacklist_value = [](const auto& result) {
            if (!result.has_value()) {
                return false;
            }
            const auto value = redis_optional_string_or_none(result);
            return value.has_value() && !value->empty();
        };

        mf::game::LoginRiskState state{
            .blacklisted = has_blacklist_value(std::get<0>(response)) ||
                has_blacklist_value(std::get<1>(response)),
        };
        const auto values = result_value_or_none(std::get<2>(response))
                                .value_or(std::vector<std::optional<std::string>>{});
        std::unordered_map<std::string, std::string> fields;
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            if (values[i] && values[i + 1]) {
                fields.emplace(*values[i], *values[i + 1]);
            }
        }
        if (const auto it = fields.find("failure_count"); it != fields.end()) {
            state.failure_count = parse_redis_i64_or_zero(it->second);
            state.captcha_required = state.failure_count >= 3;
        }
        if (const auto it = fields.find("blocked_until"); it != fields.end()) {
            const auto blocked_until = parse_redis_i64_or_zero(it->second);
            const auto now = unix_seconds(std::chrono::system_clock::now());
            if (blocked_until > now) {
                state.rate_limited = true;
                state.retry_after_seconds = blocked_until - now;
            }
        }
        if (!state.blacklisted && !state.rate_limited && !state.captcha_required && state.failure_count == 0) {
            store_clean_access_cache(cache_key);
        }
        return state;
    }

    // 登录黑名单检查同时读取 IP 和登录名两个 key；放入同一个 Redis 请求，
    // 把两个网络往返合并成一次，同时保留“任一命中即拒绝”的逻辑。
    bool is_login_blacklisted(
        const std::string& login_name,
        const std::string& client_ip) const override {
        redis::request request;
        request.push("GET", blacklist_key("ip", client_ip));
        request.push("GET", blacklist_key("login_name", login_name));
        redis::response<std::optional<std::string>, std::optional<std::string>> response;
        connection_->exec(request, response);

        const auto has_value = [](const auto& result) {
            if (!result.has_value()) {
                return false;
            }
            const auto value = redis_optional_string_or_none(result);
            return value.has_value() && !value->empty();
        };
        return has_value(std::get<0>(response)) || has_value(std::get<1>(response));
    }

    mf::game::LoginRiskState record_login_failure(
        const std::string& login_name,
        const std::string& client_ip,
        std::chrono::seconds window,
        std::int64_t captcha_threshold,
        std::int64_t rate_limit_threshold,
        std::chrono::seconds block_duration) override {
        invalidate_clean_access_cache(clean_access_cache_key(login_name, client_ip));
        const auto key = login_risk_key(login_name, client_ip);
        const auto now = std::chrono::system_clock::now();
        const auto blocked_until = unix_seconds(now + block_duration);
        redis::request request;
        request.push("HINCRBY", key, "failure_count", "1");
        request.push(
            "EXPIRE",
            key,
            std::to_string(static_cast<long long>(std::max<std::int64_t>(1, window.count()))));
        redis::response<std::int64_t, std::int64_t> response;
        connection_->exec(request, response);

        const auto failure_count = result_value_or_none(std::get<0>(response)).value_or(0);
        if (failure_count >= rate_limit_threshold) {
            redis::request block_request;
            block_request.push("HSET", key, "blocked_until", std::to_string(static_cast<long long>(blocked_until)));
            block_request.push(
                "EXPIRE",
                key,
                std::to_string(static_cast<long long>(std::max<std::int64_t>(1, window.count()))));
            redis::response<std::int64_t, std::int64_t> block_response;
            connection_->exec(block_request, block_response);
        }

        auto state = inspect_login_risk(login_name, client_ip);
        state.captcha_required = failure_count >= captcha_threshold;
        state.failure_count = failure_count;
        return state;
    }

    void clear_login_failures(const std::string& login_name, const std::string& client_ip) override {
        invalidate_clean_access_cache(clean_access_cache_key(login_name, client_ip));
        redis::request request;
        request.push("DEL", login_risk_key(login_name, client_ip));
        redis::response<std::int64_t> response;
        connection_->exec(request, response);
    }

    mf::game::LoginRiskState record_registration_attempt(
        const std::string& login_name,
        const std::string& client_ip,
        std::chrono::seconds window,
        std::int64_t max_per_login_ip,
        std::int64_t max_per_ip) override {
        const auto ttl = std::to_string(static_cast<long long>(std::max<std::int64_t>(1, window.count())));
        // Lua 保证两个计数和 TTL 设置在 Redis 内原子执行，且只在窗口首次创建时设置 TTL。
        static constexpr std::string_view script = R"lua(
local pair_count = redis.call('INCR', KEYS[1])
local ip_count = redis.call('INCR', KEYS[2])
if pair_count == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end
if ip_count == 1 then redis.call('EXPIRE', KEYS[2], ARGV[1]) end
return {pair_count, ip_count, redis.call('TTL', KEYS[1]), redis.call('TTL', KEYS[2])}
)lua";
        redis::request request;
        request.push("EVAL", std::string{script}, "2", registration_pair_key(login_name, client_ip), registration_ip_key(client_ip), ttl);
        redis::response<std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        const auto values = result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{});
        const auto value_at = [&values](std::size_t index) {
            return index < values.size() && values[index] ? parse_redis_i64_or_zero(*values[index]) : 0;
        };
        const auto pair_count = value_at(0);
        const auto ip_count = value_at(1);
        const auto pair_ttl = value_at(2);
        const auto ip_ttl = value_at(3);
        mf::game::LoginRiskState result;
        if (pair_count > max_per_login_ip || ip_count > max_per_ip) {
            result.rate_limited = true;
            result.retry_after_seconds = std::max<std::int64_t>(1, std::max(pair_ttl, ip_ttl));
        }
        return result;
    }

    void clear_registration_attempts(
        const std::string& login_name,
        const std::string& client_ip) override {
        redis::request request;
        request.push("DEL", registration_pair_key(login_name, client_ip));
        redis::response<std::int64_t> response;
        connection_->exec(request, response);
    }

    bool is_blacklisted(const std::string& subject_type, const std::string& subject_value) const override {
        redis::request request;
        request.push("GET", blacklist_key(subject_type, subject_value));
        redis::response<std::optional<std::string>> response;
        connection_->exec(request, response);

        const auto& stored_result = std::get<0>(response);
        if (!stored_result.has_value()) {
            return false;
        }

        const auto stored = redis_optional_string_or_none(stored_result);
        return stored.has_value() && !stored->empty();
    }

    mf::game::BlacklistEntry add_blacklist_entry(
        std::string subject_type,
        std::string subject_value,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> expires_at) override {
        // 黑名单属于低频管理操作，直接清空极短 clean-cache，保证本实例立即看到变更。
        clear_clean_access_cache();
        const auto entry = mf::game::BlacklistEntry{
            .entry_id = make_random_id("black_"),
            .subject_type = subject_type,
            .subject_value = subject_value,
            .reason = reason,
            .expires_at = expires_at,
        };

        redis::request request;
        if (expires_at) {
            const auto ttl = std::max<std::int64_t>(
                1,
                std::chrono::duration_cast<std::chrono::seconds>(*expires_at - std::chrono::system_clock::now())
                    .count());
            request.push("SET", blacklist_key(subject_type, subject_value), reason, "EX", std::to_string(ttl));
            request.push("SET", blacklist_meta_key(entry.entry_id), serialize_blacklist_entry(entry), "EX", std::to_string(ttl));
            request.push("SADD", blacklist_index_key(), entry.entry_id);
            redis::response<std::string, std::string, std::int64_t> response;
            connection_->exec(request, response);
        } else {
            request.push("SET", blacklist_key(subject_type, subject_value), reason);
            request.push("SET", blacklist_meta_key(entry.entry_id), serialize_blacklist_entry(entry));
            request.push("SADD", blacklist_index_key(), entry.entry_id);
            redis::response<std::string, std::string, std::int64_t> response;
            connection_->exec(request, response);
        }
        return entry;
    }

    std::vector<mf::game::BlacklistEntry> list_blacklist_entries(std::size_t limit) const override {
        redis::request request;
        request.push("SMEMBERS", blacklist_index_key());
        redis::response<std::vector<std::string>> response;
        connection_->exec(request, response);

        std::vector<mf::game::BlacklistEntry> items;
        const auto& ids = std::get<0>(response).value();
        items.reserve(std::min(limit, ids.size()));
        for (const auto& entry_id : ids) {
            redis::request get_request;
            get_request.push("GET", blacklist_meta_key(entry_id));
            redis::response<std::optional<std::string>> get_response;
            connection_->exec(get_request, get_response);
            const auto entry = deserialize_blacklist_entry(redis_optional_string_or_none(std::get<0>(get_response)));
            if (!entry) {
                continue;
            }
            items.push_back(*entry);
        }

        std::sort(
            items.begin(),
            items.end(),
            [](const mf::game::BlacklistEntry& left, const mf::game::BlacklistEntry& right) {
                return left.entry_id > right.entry_id;
            });
        if (items.size() > limit) {
            items.resize(limit);
        }
        return items;
    }

private:
    struct CleanAccessCacheShard {
        std::mutex mutex;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> expires_at_by_key;
    };

    static std::string clean_access_cache_key(
        const std::string& login_name,
        const std::string& client_ip) {
        std::string key;
        key.reserve(login_name.size() + client_ip.size() + 1);
        key.append(login_name);
        key.push_back('\0');
        key.append(client_ip);
        return key;
    }

    bool has_clean_access_cache(const std::string& key) const {
        auto& shard = clean_access_cache_[std::hash<std::string>{}(key) % clean_access_cache_.size()];
        std::scoped_lock lock{shard.mutex};
        const auto it = shard.expires_at_by_key.find(key);
        if (it == shard.expires_at_by_key.end()) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= it->second) {
            shard.expires_at_by_key.erase(it);
            return false;
        }
        return true;
    }

    void store_clean_access_cache(const std::string& key) const {
        auto& shard = clean_access_cache_[std::hash<std::string>{}(key) % clean_access_cache_.size()];
        std::scoped_lock lock{shard.mutex};
        if (shard.expires_at_by_key.size() >= 64 && !shard.expires_at_by_key.contains(key)) {
            shard.expires_at_by_key.clear();
        }
        // 只缓存 50ms：足以合并同账号/IP 的并发成功检查，同时把跨实例风控延迟限制在很小范围。
        shard.expires_at_by_key[key] = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    }

    void invalidate_clean_access_cache(const std::string& key) const {
        auto& shard = clean_access_cache_[std::hash<std::string>{}(key) % clean_access_cache_.size()];
        std::scoped_lock lock{shard.mutex};
        shard.expires_at_by_key.erase(key);
    }

    void clear_clean_access_cache() const {
        for (auto& shard : clean_access_cache_) {
            std::scoped_lock lock{shard.mutex};
            shard.expires_at_by_key.clear();
        }
    }

    std::shared_ptr<RedisExecutor> connection_;
    mutable std::array<CleanAccessCacheShard, 64> clean_access_cache_;
};

class RedisCachedChatRepository final : public mf::game::ChatRepository {
public:
    RedisCachedChatRepository(
        RedisOptions options,
        std::shared_ptr<mf::game::ChatRepository> primary,
        std::size_t window_limit)
        : connection_(shared_redis_executor(options)),
          primary_(std::move(primary)),
          window_limit_(std::max<std::size_t>(1, window_limit)) {}

    bool supports_outbox() const override { return primary_->supports_outbox(); }

    mf::game::ChatMessage append_to_outbox(mf::game::ChatMessage message) override {
        return primary_->append_to_outbox(std::move(message));
    }

    mf::game::ChatMessage append(mf::game::ChatMessage message) override {
        // 主仓储先写事实数据，Redis 只做最近窗口缓存。
        message = primary_->append(std::move(message));
        write_recent_message(message);
        return message;
    }

    std::vector<mf::game::ChatMessage> append_batch(
        std::vector<mf::game::ChatMessage> messages) override {
        // 事实数据先批量落到 MySQL；Redis 只在后台线程中刷新最近窗口，
        // 即使缓存写入失败，也不会影响消息已经持久化的事实。
        messages = primary_->append_batch(std::move(messages));
        for (const auto& message : messages) {
            write_recent_message(message);
        }
        return messages;
    }

    mf::game::ChatOutboxBatchResult drain_outbox_batch(std::size_t limit) override {
        auto result = primary_->drain_outbox_batch(limit);
        for (const auto& message : result.messages) {
            write_recent_message(message);
        }
        return result;
    }

    mf::game::ChatOutboxHealth outbox_health() const override {
        return primary_->outbox_health();
    }

    std::size_t retry_failed_outbox(std::size_t limit) override {
        return primary_->retry_failed_outbox(limit);
    }

    std::optional<mf::game::ChatMessage> recall(
        const std::string& channel_id,
        const std::string& message_id,
        const std::string& recalled_by_player_id) override {
        auto recalled = primary_->recall(channel_id, message_id, recalled_by_player_id);
        if (recalled) {
            refresh_recent_cache(channel_id, primary_->recent_messages(channel_id, window_limit_));
        }
        return recalled;
    }

    std::vector<mf::game::ChatMessage> recent_messages(const std::string& channel_id, std::size_t limit) const override {
        const auto bounded_limit = std::max<std::size_t>(1, std::min(limit, window_limit_));
        if (const auto cached = read_recent_cache(channel_id, bounded_limit)) {
            return *cached;
        }

        // 缓存未命中或不可信时回源 MySQL，再把这一小段最近窗口回填到 Redis。
        auto messages = primary_->recent_messages(channel_id, bounded_limit);
        refresh_recent_cache(channel_id, messages);
        return messages;
    }

    mf::game::ChatHistoryPage history_page(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const override {
        // 深分页必须回源 MySQL；Redis 最近窗口只承载首屏热缓存，
        // 避免用固定大小的 LRANGE 窗口伪造 has_more/next_cursor。
        return primary_->history_page(channel_id, cursor, limit);
    }

    std::vector<mf::game::ChatMessage> latest_messages(
        const std::vector<std::string>& channel_ids,
        std::size_t limit) const override {
        if (channel_ids.empty()) {
            return {};
        }

        if (limit > 1) {
            return primary_->latest_messages(channel_ids, limit);
        }
        const auto bounded_limit = std::size_t{1};

        redis::request request;
        std::vector<std::string> cache_keys;
        cache_keys.reserve(channel_ids.size());
        for (const auto& channel_id : channel_ids) {
            cache_keys.push_back(latest_message_cache_key(channel_id));
        }
        request.push_range("MGET", cache_keys);

        redis::response<std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);
        const auto values = result_value_or_none(std::get<0>(response))
                                .value_or(std::vector<std::optional<std::string>>{});

        std::vector<std::string> cold_channels;
        std::vector<mf::game::ChatMessage> messages;
        messages.reserve(channel_ids.size());
        for (std::size_t index = 0; index < channel_ids.size(); ++index) {
            if (index >= values.size() || !values[index]) {
                cold_channels.push_back(channel_ids[index]);
                increment_redis_counter("mf_chat_latest_cache_total", "miss");
                continue;
            }
            if (values[index]->empty()) {
                increment_redis_counter("mf_chat_latest_cache_total", "hit_empty");
                continue;
            }
            try {
                if (const auto message = deserialize_chat_message(values[index])) {
                    messages.push_back(*message);
                    increment_redis_counter("mf_chat_latest_cache_total", "hit");
                    continue;
                }
            } catch (const std::exception&) {
                // 格式不兼容或缓存损坏时回源权威数据库修复缓存。
            }
            cold_channels.push_back(channel_ids[index]);
            increment_redis_counter("mf_chat_latest_cache_total", "miss");
        }

        // 只有冷频道回源 MySQL；热频道不会再触发好友页的历史消息查询。
        if (!cold_channels.empty()) {
            increment_redis_counter("mf_chat_latest_cache_total", "mysql_fallback");
            auto fetched = primary_->latest_messages(cold_channels, bounded_limit);
            std::unordered_map<std::string, mf::game::ChatMessage> by_channel;
            by_channel.reserve(fetched.size());
            for (const auto& message : fetched) {
                by_channel.emplace(message.channel.channel_id, message);
                messages.push_back(message);
            }

            redis::request warm_request;
            std::vector<std::string> warm_arguments;
            warm_arguments.reserve(2 + cold_channels.size() * 4);
            warm_arguments.emplace_back(kWriteLatestChatMessagesScript);
            warm_arguments.push_back(std::to_string(cold_channels.size()));
            for (const auto& channel_id : cold_channels) {
                warm_arguments.push_back(latest_message_cache_key(channel_id));
            }
            for (const auto& channel_id : cold_channels) {
                const auto found = by_channel.find(channel_id);
                if (found == by_channel.end()) {
                    warm_arguments.emplace_back();
                    warm_arguments.emplace_back("0");
                    warm_arguments.emplace_back();
                } else {
                    warm_arguments.push_back(serialize_chat_message(found->second));
                    warm_arguments.push_back(std::to_string(unix_seconds(found->second.sent_at)));
                    warm_arguments.push_back(found->second.message_id);
                }
            }
            warm_arguments.emplace_back("3600");
            warm_request.push_range("EVAL", warm_arguments);
            redis::response<std::int64_t> warm_response;
            connection_->exec(warm_request, warm_response);
        }
        return messages;
    }

private:
    std::optional<std::vector<mf::game::ChatMessage>> read_recent_cache(
        const std::string& channel_id,
        std::size_t limit) const {
        redis::request request;
        request.push("LRANGE", recent_messages_key(channel_id), "0", std::to_string(limit - 1));
        request.push("EXISTS", recent_messages_warm_key(channel_id));
        redis::response<std::vector<std::optional<std::string>>, std::int64_t> response;
        connection_->exec(request, response);

        const auto cached_messages = decode_chat_messages(
            result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{}),
            true);
        const auto warm = result_value_or_none(std::get<1>(response)).value_or(0) > 0;

        if (!cached_messages.empty() && (cached_messages.size() >= limit || warm)) {
            return cached_messages;
        }
        if (cached_messages.empty() && warm) {
            return std::vector<mf::game::ChatMessage>{};
        }
        return std::nullopt;
    }

    void write_recent_message(const mf::game::ChatMessage& message) const {
        redis::request request;
        request.push("LPUSH", recent_messages_key(message.channel.channel_id), serialize_chat_message(message));
        request.push(
            "LTRIM",
            recent_messages_key(message.channel.channel_id),
            "0",
            std::to_string(window_limit_ - 1));
        request.push("SET", recent_messages_warm_key(message.channel.channel_id), "1", "EX", "3600");
        const std::vector<std::string> latest_arguments{
            std::string{kWriteLatestChatMessagesScript},
            "1",
            latest_message_cache_key(message.channel.channel_id),
            serialize_chat_message(message),
            std::to_string(unix_seconds(message.sent_at)),
            message.message_id,
            "3600",
        };
        request.push_range("EVAL", latest_arguments);
        redis::response<std::int64_t, std::string, std::string, std::int64_t> response;
        connection_->exec(request, response);
    }

    void refresh_recent_cache(
        const std::string& channel_id,
        const std::vector<mf::game::ChatMessage>& messages) const {
        if (messages.empty()) {
            redis::request empty_request;
            empty_request.push("DEL", recent_messages_key(channel_id));
            empty_request.push("SET", recent_messages_warm_key(channel_id), "1", "EX", "3600");
            const std::vector<std::string> latest_arguments{
                std::string{kWriteLatestChatMessagesScript}, "1", latest_message_cache_key(channel_id), "", "0", "", "3600"};
            empty_request.push_range("EVAL", latest_arguments);
            redis::response<std::int64_t, std::string, std::int64_t> empty_response;
            connection_->exec(empty_request, empty_response);
            return;
        }

        redis::request delete_request;
        delete_request.push("DEL", recent_messages_key(channel_id));
        redis::response<std::int64_t> delete_response;
        connection_->exec(delete_request, delete_response);

        // primary_->recent_messages() 返回顺序是“旧 -> 新”；
        // 这里逐条 LPUSH 后，Redis 列表头部自然就会变成“最新消息”，
        // 和 append() 的缓存写入布局保持一致。
        for (const auto& message : messages) {
            redis::request push_request;
            push_request.push("LPUSH", recent_messages_key(channel_id), serialize_chat_message(message));
            redis::response<std::int64_t> push_response;
            connection_->exec(push_request, push_response);
        }

        redis::request tail_request;
        tail_request.push("LTRIM", recent_messages_key(channel_id), "0", std::to_string(window_limit_ - 1));
        tail_request.push("SET", recent_messages_warm_key(channel_id), "1", "EX", "3600");
        if (!messages.empty()) {
            const auto& latest = messages.back();
            const std::vector<std::string> latest_arguments{
                std::string{kWriteLatestChatMessagesScript},
                "1",
                latest_message_cache_key(channel_id),
                serialize_chat_message(latest),
                std::to_string(unix_seconds(latest.sent_at)),
                latest.message_id,
                "3600",
            };
            tail_request.push_range("EVAL", latest_arguments);
        }
        redis::response<std::string, std::string, std::int64_t> tail_response;
        connection_->exec(tail_request, tail_response);
    }

    std::shared_ptr<RedisExecutor> connection_;
    std::shared_ptr<mf::game::ChatRepository> primary_;
    std::size_t window_limit_;
};

class RedisCachedAnnouncementRepository final : public mf::game::AnnouncementRepository {
public:
    RedisCachedAnnouncementRepository(
        RedisOptions options,
        std::shared_ptr<mf::game::AnnouncementRepository> primary,
        std::size_t window_limit)
        : connection_(shared_redis_executor(options)),
          primary_(std::move(primary)),
          window_limit_(std::max<std::size_t>(1, window_limit)) {}

    mf::game::AnnouncementEntry publish(
        mf::game::ChatChannel channel,
        std::string operator_id,
        std::string title,
        std::string body,
        std::chrono::system_clock::time_point published_at,
        std::optional<std::chrono::system_clock::time_point> expires_at) override {
        auto entry = primary_->publish(
            std::move(channel),
            std::move(operator_id),
            std::move(title),
            std::move(body),
            published_at,
            expires_at);
        write_recent_announcement(entry);
        return entry;
    }

    std::vector<mf::game::AnnouncementEntry> list_recent(
        std::size_t limit,
        const std::optional<mf::game::ChatChannelKind>& kind_filter,
        const std::optional<std::string>& scope_filter,
        std::chrono::system_clock::time_point now) const override {
        const auto kind = kind_filter.value_or(mf::game::ChatChannelKind::lobby);
        const auto scope = scope_filter.value_or(std::string{"global"});
        const auto bounded_limit = std::max<std::size_t>(1, std::min(limit, window_limit_));
        if (const auto cached = read_recent_cache(kind, scope, bounded_limit, now)) {
            return *cached;
        }
        auto items = primary_->list_recent(bounded_limit, kind_filter, scope_filter, now);
        refresh_recent_cache(kind, scope, items);
        return items;
    }

private:
    std::optional<std::vector<mf::game::AnnouncementEntry>> read_recent_cache(
        mf::game::ChatChannelKind kind,
        const std::string& scope,
        std::size_t limit,
        std::chrono::system_clock::time_point now) const {
        redis::request request;
        const auto kind_text = std::string{mf::chat::to_string(kind)};
        request.push("LRANGE", recent_announcements_key(kind_text, scope), "0", std::to_string(limit - 1));
        request.push("EXISTS", recent_announcements_warm_key(kind_text, scope));
        redis::response<std::vector<std::optional<std::string>>, std::int64_t> response;
        connection_->exec(request, response);
        auto items = decode_announcements(
            result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{}));
        items.erase(
            std::remove_if(
                items.begin(),
                items.end(),
                [now](const mf::game::AnnouncementEntry& item) {
                    return item.expires_at && *item.expires_at < now;
                }),
            items.end());
        const auto warm = result_value_or_none(std::get<1>(response)).value_or(0) > 0;
        if (!items.empty() && (items.size() >= limit || warm)) {
            return items;
        }
        if (items.empty() && warm) {
            return std::vector<mf::game::AnnouncementEntry>{};
        }
        return std::nullopt;
    }

    void write_recent_announcement(const mf::game::AnnouncementEntry& entry) const {
        const auto kind_text = std::string{mf::chat::to_string(entry.channel.kind)};
        const auto scope = entry.channel.scope_id.empty() ? std::string{"global"} : entry.channel.scope_id;
        redis::request request;
        request.push("LPUSH", recent_announcements_key(kind_text, scope), serialize_announcement(entry));
        request.push("LTRIM", recent_announcements_key(kind_text, scope), "0", std::to_string(window_limit_ - 1));
        request.push("SET", recent_announcements_warm_key(kind_text, scope), "1", "EX", "3600");
        redis::response<std::int64_t, std::string, std::string> response;
        connection_->exec(request, response);
    }

    void refresh_recent_cache(
        mf::game::ChatChannelKind kind,
        const std::string& scope,
        const std::vector<mf::game::AnnouncementEntry>& items) const {
        const auto kind_text = std::string{mf::chat::to_string(kind)};
        if (items.empty()) {
            redis::request empty_request;
            empty_request.push("DEL", recent_announcements_key(kind_text, scope));
            empty_request.push("SET", recent_announcements_warm_key(kind_text, scope), "1", "EX", "3600");
            redis::response<std::int64_t, std::string> empty_response;
            connection_->exec(empty_request, empty_response);
            return;
        }
        redis::request delete_request;
        delete_request.push("DEL", recent_announcements_key(kind_text, scope));
        redis::response<std::int64_t> delete_response;
        connection_->exec(delete_request, delete_response);
        for (const auto& item : items) {
            redis::request push_request;
            push_request.push("LPUSH", recent_announcements_key(kind_text, scope), serialize_announcement(item));
            redis::response<std::int64_t> push_response;
            connection_->exec(push_request, push_response);
        }
        redis::request tail_request;
        tail_request.push("LTRIM", recent_announcements_key(kind_text, scope), "0", std::to_string(window_limit_ - 1));
        tail_request.push("SET", recent_announcements_warm_key(kind_text, scope), "1", "EX", "3600");
        redis::response<std::string, std::string> tail_response;
        connection_->exec(tail_request, tail_response);
    }

    std::shared_ptr<RedisExecutor> connection_;
    std::shared_ptr<mf::game::AnnouncementRepository> primary_;
    std::size_t window_limit_;
};

struct SScanResult {
    std::string cursor;
    std::vector<std::string> members;
};

// 解析 SSCAN 的 RESP3 通用树：返回 [cursor, [member...]] 两个部分。
SScanResult parse_sscan_tree(const redis::resp3::tree& tree) {
    SScanResult result;
    result.cursor = "0";
    if (tree.size() < 2 || !redis::resp3::is_aggregate(tree[0].data_type)) {
        return result;
    }

    // 第一个标量子节点是游标。
    std::size_t index = 1;
    if (index < tree.size() && !redis::resp3::is_aggregate(tree[index].data_type)) {
        result.cursor = tree[index].value;
        ++index;
    }

    // 第一个聚合子节点的所有子节点是成员。
    if (index < tree.size() && redis::resp3::is_aggregate(tree[index].data_type)) {
        const auto count = tree[index].aggregate_size;
        const auto depth = tree[index].depth;
        ++index;
        std::size_t collected = 0;
        while (index < tree.size() && collected < count && tree[index].depth > depth) {
            if (!tree[index].value.empty()) {
                result.members.push_back(tree[index].value);
            }
            ++index;
            ++collected;
        }
    }
    return result;
}

class RedisChatStateService final : public mf::chat::ChatStateService {
public:
    explicit RedisChatStateService(RedisOptions options)
        : connection_(shared_redis_executor(options)) {}

    std::size_t mark_online(const std::string& player_id) override {
        redis::request request;
        request.push("INCR", online_count_key(player_id));
        request.push("SADD", online_players_key(), player_id);
        redis::response<std::int64_t, std::int64_t> response;
        connection_->exec(request, response);
        return static_cast<std::size_t>(result_value_or_none(std::get<0>(response)).value_or(0));
    }

    std::size_t mark_offline(const std::string& player_id) override {
        const auto count = decrement_counter(online_count_key(player_id));
        if (count == 0) {
            redis::request request;
            request.push("SREM", online_players_key(), player_id);
            redis::response<std::int64_t> response;
            connection_->exec(request, response);
        }
        return count;
    }

    std::size_t join_channel(const mf::game::ChatChannel& channel, const std::string& player_id) override {
        redis::request request;
        request.push("INCR", channel_member_count_key(channel.channel_id, player_id));
        request.push("SADD", channel_members_key(channel.channel_id), player_id);
        request.push("HDEL", unread_hash_key(player_id), channel.channel_id);
        redis::response<std::int64_t, std::int64_t, std::int64_t> response;
        connection_->exec(request, response);
        return static_cast<std::size_t>(result_value_or_none(std::get<0>(response)).value_or(0));
    }

    std::size_t leave_channel(const std::string& channel_id, const std::string& player_id) override {
        const auto count = decrement_counter(channel_member_count_key(channel_id, player_id));
        if (count == 0) {
            redis::request request;
            request.push("SREM", channel_members_key(channel_id), player_id);
            redis::response<std::int64_t> response;
            connection_->exec(request, response);
        }
        return count;
    }

    void on_message_persisted(const mf::game::ChatMessage& message) override {
        if (message.channel.kind != mf::game::ChatChannelKind::friend_direct) {
            return;
        }

        const auto participants = friend_channel_participants(message.channel);
        for (const auto& player_id : participants) {
            if (player_id == message.sender_player_id) {
                continue;
            }

            const std::vector<std::string> arguments{
                std::string{kRecordFriendMessageStateScript},
                "3",
                unread_hash_key(player_id),
                offline_messages_key(player_id),
                online_players_key(),
                player_id,
                message.channel.channel_id,
                serialize_chat_message(message),
            };
            redis::request request;
            request.push_range("EVAL", arguments);
            redis::response<std::int64_t> response;
            connection_->exec(request, response);
        }
    }

    void mark_channel_read(const std::string& player_id, const std::string& channel_id) override {
        redis::request request;
        request.push("HDEL", unread_hash_key(player_id), channel_id);
        redis::response<std::int64_t> response;
        connection_->exec(request, response);
    }

    std::vector<mf::game::ChannelUnreadCount> list_unread_counts(const std::string& player_id) const override {
        redis::request request;
        request.push("HGETALL", unread_hash_key(player_id));
        redis::response<std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        std::vector<mf::game::ChannelUnreadCount> items;
        const auto values = result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{});
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            if (!values[i] || !values[i + 1]) {
                continue;
            }
            items.push_back(mf::game::ChannelUnreadCount{
                .channel_id = *values[i],
                .unread_count = parse_redis_i64_or_zero(*values[i + 1]),
            });
        }
        return items;
    }

    // HMGET 只读取本页频道的未读字段，不把玩家其他好友频道的 Hash 内容搬到进程。
    std::vector<mf::game::ChannelUnreadCount> list_unread_counts_for_channels(
        const std::string& player_id,
        const std::vector<std::string>& channel_ids) const override {
        std::vector<mf::game::ChannelUnreadCount> items;
        if (channel_ids.empty()) {
            return items;
        }

        redis::request request;
        request.push_range("HMGET", unread_hash_key(player_id), channel_ids);
        redis::response<std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        const auto values = result_value_or_none(std::get<0>(response))
            .value_or(std::vector<std::optional<std::string>>{});
        items.reserve(std::min(channel_ids.size(), values.size()));
        for (std::size_t index = 0; index < channel_ids.size() && index < values.size(); ++index) {
            if (!values[index]) {
                continue;
            }
            items.push_back(mf::game::ChannelUnreadCount{
                .channel_id = channel_ids[index],
                .unread_count = parse_redis_i64_or_zero(*values[index]),
            });
        }
        return items;
    }

    std::vector<mf::game::ChatMessage> pull_offline_messages(
        const std::string& player_id,
        std::size_t limit) override {
        const auto bounded_limit = std::max<std::size_t>(1, limit);

        redis::request request;
        request.push("LRANGE", offline_messages_key(player_id), "0", std::to_string(bounded_limit - 1));
        request.push("LTRIM", offline_messages_key(player_id), std::to_string(bounded_limit), "-1");
        redis::response<std::vector<std::optional<std::string>>, std::string> response;
        connection_->exec(request, response);

        auto messages = decode_chat_messages(
            result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{}),
            false);
        for (const auto& message : messages) {
            mark_channel_read(player_id, message.channel.channel_id);
        }
        return messages;
    }

    bool is_online(const std::string& player_id) const override {
        redis::request request;
        request.push("SISMEMBER", online_players_key(), player_id);
        redis::response<std::int64_t> response;
        connection_->exec(request, response);
        return result_value_or_none(std::get<0>(response)).value_or(0) > 0;
    }

    std::vector<mf::game::FriendPresence> list_friend_presence(
        const std::vector<std::string>& player_ids) const override {
        std::vector<mf::game::FriendPresence> items;
        if (player_ids.empty()) {
            return items;
        }

        redis::request request;
        request.push_range("SMISMEMBER", online_players_key(), player_ids);

        std::vector<std::string> count_keys;
        count_keys.reserve(player_ids.size());
        for (const auto& player_id : player_ids) {
            count_keys.push_back(online_count_key(player_id));
        }
        request.push_range("MGET", count_keys);

        redis::response<std::vector<std::int64_t>, std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        const auto online_flags = result_value_or_none(std::get<0>(response)).value_or(std::vector<std::int64_t>{});
        const auto count_values =
            result_value_or_none(std::get<1>(response)).value_or(std::vector<std::optional<std::string>>{});
        items.reserve(player_ids.size());
        for (std::size_t i = 0; i < player_ids.size(); ++i) {
            items.push_back(mf::game::FriendPresence{
                .player_id = player_ids[i],
                .online = i < online_flags.size() && online_flags[i] > 0,
                .connection_count =
                    (i < count_values.size() && count_values[i] && !count_values[i]->empty())
                        ? parse_redis_i64_or_zero(*count_values[i])
                        : 0,
            });
        }
        return items;
    }

    mf::chat::FriendOverviewState list_friend_overview_state(
        const std::string& player_id,
        const std::vector<std::string>& friend_player_ids,
        const std::vector<std::string>& channel_ids) const override {
        mf::chat::FriendOverviewState state;
        if (friend_player_ids.empty() && channel_ids.empty()) {
            return state;
        }

        redis::request request;
        request.push_range("SMISMEMBER", online_players_key(), friend_player_ids);

        std::vector<std::string> count_keys;
        count_keys.reserve(friend_player_ids.size());
        for (const auto& friend_player_id : friend_player_ids) {
            count_keys.push_back(online_count_key(friend_player_id));
        }
        request.push_range("MGET", count_keys);
        request.push_range("HMGET", unread_hash_key(player_id), channel_ids);

        redis::response<
            std::vector<std::int64_t>,
            std::vector<std::optional<std::string>>,
            std::vector<std::optional<std::string>>> response;
        connection_->exec(request, response);

        const auto online_flags = result_value_or_none(std::get<0>(response))
            .value_or(std::vector<std::int64_t>{});
        const auto count_values = result_value_or_none(std::get<1>(response))
            .value_or(std::vector<std::optional<std::string>>{});
        const auto unread_values = result_value_or_none(std::get<2>(response))
            .value_or(std::vector<std::optional<std::string>>{});

        state.presence.reserve(friend_player_ids.size());
        for (std::size_t i = 0; i < friend_player_ids.size(); ++i) {
            state.presence.push_back(mf::game::FriendPresence{
                .player_id = friend_player_ids[i],
                .online = i < online_flags.size() && online_flags[i] > 0,
                .connection_count =
                    i < count_values.size() && count_values[i] && !count_values[i]->empty()
                        ? parse_redis_i64_or_zero(*count_values[i])
                        : 0,
            });
        }
        state.unread_counts.reserve(channel_ids.size());
        for (std::size_t i = 0; i < channel_ids.size() && i < unread_values.size(); ++i) {
            if (unread_values[i]) {
                state.unread_counts.push_back(mf::game::ChannelUnreadCount{
                    .channel_id = channel_ids[i],
                    .unread_count = parse_redis_i64_or_zero(*unread_values[i]),
                });
            }
        }
        return state;
    }

    mf::chat::ChannelMembersPage list_channel_members(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const override {
        const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 1000);
        mf::chat::ChannelMembersPage page;

        // 独立成员总数：SCARD O(1)，避免为显示数量而全量 SMEMBERS。
        {
            redis::request count_request;
            count_request.push("SCARD", channel_members_key(channel_id));
            redis::response<std::int64_t> count_response;
            connection_->exec(count_request, count_response);
            page.total_count = result_value_or_none(std::get<0>(count_response)).value_or(0);
        }

        // 分页成员：SSCAN 游标式迭代。SSCAN 的游标粒度与页大小不一致，
        // 因此本页会完整包含每个批次，size 最多到 bounded_limit + 一个批次，
        // 但绝不会因中途截断批次而丢成员。
        std::string scan_cursor = cursor.empty() ? std::string{"0"} : std::string{cursor};
        do {
            redis::request scan_request;
            scan_request.push(
                "SSCAN",
                channel_members_key(channel_id),
                scan_cursor,
                "COUNT",
                std::to_string(bounded_limit));
            redis::response<redis::resp3::tree> scan_response;
            connection_->exec(scan_request, scan_response);
            const auto tree =
                result_value_or_none(std::get<0>(scan_response)).value_or(redis::resp3::tree{});
            const auto scan_result = parse_sscan_tree(tree);
            scan_cursor = scan_result.cursor.empty() ? std::string{"0"} : scan_result.cursor;
            for (const auto& member : scan_result.members) {
                page.members.push_back(member);
            }
        } while (scan_cursor != "0" && page.members.size() < bounded_limit);

        page.has_more = scan_cursor != "0";
        page.next_cursor = scan_cursor == "0" ? std::string{} : scan_cursor;
        return page;
    }

private:
    std::size_t decrement_counter(const std::string& key) const {
        redis::request request;
        request.push("DECR", key);
        redis::response<std::int64_t> response;
        connection_->exec(request, response);

        const auto raw_value = result_value_or_none(std::get<0>(response)).value_or(0);
        if (raw_value <= 0) {
            redis::request cleanup_request;
            cleanup_request.push("DEL", key);
            redis::response<std::int64_t> cleanup_response;
            connection_->exec(cleanup_request, cleanup_response);
            return 0;
        }
        return static_cast<std::size_t>(raw_value);
    }

    std::shared_ptr<RedisExecutor> connection_;
};

class RedisSessionRevocationSubscriber final : public mf::storage::SessionRevocationSubscriber {
public:
    RedisSessionRevocationSubscriber(
        RedisOptions options,
        std::function<void(std::string, std::optional<std::string>)> on_player_revoked)
        : options_(std::move(options)), on_player_revoked_(std::move(on_player_revoked)) {}

    ~RedisSessionRevocationSubscriber() override {
        stop();
    }

    void start() override {
        if (started_.exchange(true)) {
            return;
        }

        response_ = std::make_shared<redis::generic_flat_response>();
        connection_ = std::make_shared<redis::connection>(ioc_);
        connection_->set_receive_response(*response_);

        asio::co_spawn(ioc_, receive_loop(connection_, response_), asio::detached);

        const auto config = make_config();
        thread_ = std::thread([this, config]() mutable {
            connection_->async_run(config, asio::consign(asio::detached, connection_));
            ioc_.run();
        });
    }

    void stop() override {
        if (!started_.exchange(false)) {
            return;
        }

        if (connection_) {
            connection_->cancel();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        connection_.reset();
        response_.reset();
    }

private:
    redis::config make_config() const {
        return make_redis_config(options_);
    }

    asio::awaitable<void> receive_loop(
        std::shared_ptr<redis::connection> connection,
        std::shared_ptr<redis::generic_flat_response> response) {
        redis::request subscribe_request;
        subscribe_request.subscribe({kSessionRevocationChannel});
        co_await connection->async_exec(subscribe_request);

        while (connection->will_reconnect()) {
            auto [ec] = co_await connection->async_receive2(asio::as_tuple);
            if (ec) {
                break;
            }
            if (response->has_error()) {
                break;
            }

            for (const redis::push_view& event : redis::push_parser(response->value())) {
                if (event.channel != kSessionRevocationChannel || event.payload.empty()) {
                    continue;
                }

                try {
                    const auto object = boost::json::parse(event.payload).as_object();
                    const auto player_it = object.find("player_id");
                    if (player_it == object.end() || !player_it->value().is_string()) {
                        continue;
                    }

                    std::optional<std::string> except_session_id;
                    const auto session_it = object.find("session_id");
                    if (session_it != object.end() && session_it->value().is_string()) {
                        except_session_id = boost::json::value_to<std::string>(session_it->value());
                    }

                    on_player_revoked_(
                        boost::json::value_to<std::string>(player_it->value()),
                        std::move(except_session_id));
                } catch (...) {
                    // 单条广播损坏不应终止订阅循环。
                }
            }

            response->value().clear();
        }
    }

    RedisOptions options_;
    std::function<void(std::string, std::optional<std::string>)> on_player_revoked_;
    asio::io_context ioc_{1};
    std::shared_ptr<redis::connection> connection_;
    std::shared_ptr<redis::generic_flat_response> response_;
    std::thread thread_;
    std::atomic<bool> started_{false};
};

}  // 匿名命名空间

std::shared_ptr<mf::game::SessionRepository> make_redis_session_repository(const RedisOptions& options) {
    return std::make_shared<RedisSessionRepository>(options);
}

std::shared_ptr<SessionRevocationSubscriber> make_redis_session_revocation_subscriber(
    const RedisOptions& options,
    std::function<void(std::string, std::optional<std::string>)> on_player_revoked) {
    return std::make_shared<RedisSessionRevocationSubscriber>(
        options,
        std::move(on_player_revoked));
}

std::shared_ptr<mf::game::AuthSecurityRepository> make_redis_auth_security_repository(const RedisOptions& options) {
    return std::make_shared<RedisAuthSecurityRepository>(options);
}

std::shared_ptr<mf::game::ChatRepository> make_redis_cached_chat_repository(
    const RedisOptions& options,
    std::shared_ptr<mf::game::ChatRepository> primary,
    std::size_t window_limit) {
    return std::make_shared<RedisCachedChatRepository>(options, std::move(primary), window_limit);
}

std::shared_ptr<mf::game::AnnouncementRepository> make_redis_cached_announcement_repository(
    const RedisOptions& options,
    std::shared_ptr<mf::game::AnnouncementRepository> primary,
    std::size_t window_limit) {
    return std::make_shared<RedisCachedAnnouncementRepository>(options, std::move(primary), window_limit);
}

std::shared_ptr<mf::chat::ChatStateService> make_redis_chat_state_service(const RedisOptions& options) {
    return std::make_shared<RedisChatStateService>(options);
}

std::optional<AnnouncementCacheDebugSnapshot> inspect_announcement_cache(
    const RedisOptions& options,
    mf::game::ChatChannelKind kind,
    std::string scope_id,
    std::size_t limit) {
    if (scope_id.empty()) {
        return std::nullopt;
    }

    const auto kind_text = std::string{mf::chat::to_string(kind)};
    const auto recent_key = recent_announcements_key(kind_text, scope_id);
    const auto warm_key = recent_announcements_warm_key(kind_text, scope_id);

    redis::request request;
    request.push("LRANGE", recent_key, "0", std::to_string(limit == 0 ? 0 : limit - 1));
    request.push("EXISTS", warm_key);

    redis::response<std::vector<std::optional<std::string>>, std::int64_t> response;
    shared_redis_executor(options)->exec(request, response);

    return AnnouncementCacheDebugSnapshot{
        .channel_kind = kind_text,
        .scope_id = std::move(scope_id),
        .recent_key = std::move(recent_key),
        .warm_key = std::move(warm_key),
        .warm = result_value_or_none(std::get<1>(response)).value_or(0) > 0,
        .items = decode_announcements(
            result_value_or_none(std::get<0>(response)).value_or(std::vector<std::optional<std::string>>{})),
    };
}

}  // namespace mf::storage 命名空间
