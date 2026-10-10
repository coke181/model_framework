#pragma once

// 文件职责：
// 定义聊天服务，负责频道订阅、消息发布和历史查询。
// 调用流程：
// ChatGatewaySession 加入频道时调用 subscribe() ->
// 客户端发言时调用 publish() -> ChatService 落存并向在线订阅者扇出 ->
// 客户端补历史时调用 history()。

#include "mf/game/models.hpp"
#include "mf/game/repositories.hpp"
#include "mf/chat/state.hpp"
#include "mf/core/metrics.hpp"
#include "mf/core/logger.hpp"

#include <functional>
#include <array>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mf::chat {

class ChatService {
public:
    struct PublishResult {
        bool ok = false;
        std::string code;
        std::string message;
        std::optional<mf::game::ChatMessage> delivered_message;
    };

    // DeliveryFn 表示一个在线订阅端点，通常就是一个 websocket session。
    using DeliveryFn = std::function<void(const mf::game::ChatMessage&)>;

    ChatService(
        std::shared_ptr<mf::game::ChatRepository> repository,
        std::shared_ptr<ChatStateService> state_service,
        std::shared_ptr<mf::game::FriendRepository> friend_repository,
        std::shared_ptr<mf::game::ChatModerationRepository> moderation_repository,
        std::size_t persistence_queue_limit = 4096,
        std::size_t persistence_batch_size = 64,
        std::shared_ptr<mf::core::MetricsRegistry> metrics = {},
        std::shared_ptr<mf::core::Logger> logger = {},
        std::size_t persistence_max_retries = 5);
    ~ChatService();

    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    std::size_t subscribe(mf::game::ChatChannel channel, DeliveryFn delivery);
    void unsubscribe(const std::string& channel_id, std::size_t subscription_id);
    // publish() 是消息扇出的边界点：先落存，再投递给当前在线订阅者。
    // 发布锁按频道分片：同一个频道保持落库和广播顺序，
    // 不同频道可以同时写入 MySQL/Redis，避免全局锁限制吞吐量。
    PublishResult publish(mf::game::ChatMessage message);
    std::optional<mf::game::ChatMessage> recall(
        const std::string& channel_id,
        const std::string& message_id,
        const std::string& operator_player_id,
        bool gm_override = false);
    mf::game::ChatMuteEntry mute_player(
        const std::string& channel_id,
        const std::string& muted_player_id,
        const std::string& muted_by_player_id,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> muted_until);
    void clear_mute(const std::string& channel_id, const std::string& muted_player_id);
    std::vector<mf::game::ChatMessage> history(const std::string& channel_id, std::size_t limit) const;
    mf::game::ChatHistoryPage history_page(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const;
    std::vector<mf::game::ChatMessage> latest_messages(
        const std::vector<std::string>& channel_ids,
        std::size_t limit) const;

private:
    struct Subscription {
        // id 是退订时用来精确删除当前订阅记录的句柄；
        // delivery 才是真正负责把消息送到这个订阅者的函数。
        std::size_t id;
        mf::game::ChatChannel channel;
        DeliveryFn delivery;
    };

    // 一个频道的订阅记录只有在加入/退出时才会变化。
    // delivery_snapshot 用于广播快路径：发送消息时不再复制整个订阅数组和
    // std::function，避免每条消息都产生临时容器和堆分配。
    struct ChannelSubscriptions {
        std::vector<Subscription> subscriptions;
        std::shared_ptr<const std::vector<DeliveryFn>> delivery_snapshot;
    };

    void broadcast(const mf::game::ChatMessage& message);
    PublishResult validate_publish(const mf::game::ChatMessage& message) const;
    std::mutex& publish_mutex_for(std::string_view channel_id);
    void persistence_loop(std::stop_token stop_token);
    bool enqueue_persistence(mf::game::ChatMessage message);
    void persist_batch(std::vector<mf::game::ChatMessage> batch);

    std::shared_ptr<mf::game::ChatRepository> repository_;
    std::shared_ptr<ChatStateService> state_service_;
    std::shared_ptr<mf::game::FriendRepository> friend_repository_;
    std::shared_ptr<mf::game::ChatModerationRepository> moderation_repository_;
    std::shared_ptr<mf::core::MetricsRegistry> metrics_;
    std::shared_ptr<mf::core::Logger> logger_;
    const std::size_t persistence_max_retries_;
    static constexpr std::size_t kPublishShardCount = 64;
    std::array<std::mutex, kPublishShardCount> publish_mutexes_;
    mutable std::mutex mutex_;
    std::size_t next_subscription_id_ = 1;
    std::unordered_map<std::string, ChannelSubscriptions> subscriptions_;

    // 广播线程只负责校验、更新实时状态和投递；持久化由独立线程批量处理。
    const std::size_t persistence_queue_limit_;
    const std::size_t persistence_batch_size_;
    mutable std::mutex persistence_mutex_;
    std::condition_variable_any persistence_condition_;
    std::deque<mf::game::ChatMessage> persistence_queue_;
    std::size_t persistence_in_flight_ = 0;
    bool persistence_wakeup_ = false;
    std::jthread persistence_worker_;
};

}  // namespace mf::chat 命名空间
