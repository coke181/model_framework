#include "mf/chat/chat_service.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现聊天服务本身：订阅管理、消息扇出、历史查询。
// 调用流程：
// GatewaySession 调用 subscribe()/unsubscribe() 管理在线连接 ->
// 调用 publish() 先落存再广播 ->
// 调用 history() 从 ChatRepository 拉取最近消息。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace mf::chat {

ChatService::ChatService(
    std::shared_ptr<mf::game::ChatRepository> repository,
    std::shared_ptr<ChatStateService> state_service,
    std::shared_ptr<mf::game::FriendRepository> friend_repository,
    std::shared_ptr<mf::game::ChatModerationRepository> moderation_repository,
    std::size_t persistence_queue_limit,
    std::size_t persistence_batch_size,
    std::shared_ptr<mf::core::MetricsRegistry> metrics,
    std::shared_ptr<mf::core::Logger> logger,
    std::size_t persistence_max_retries)
    : repository_(std::move(repository)),
      state_service_(std::move(state_service)),
      friend_repository_(std::move(friend_repository)),
      moderation_repository_(std::move(moderation_repository)),
      metrics_(std::move(metrics)),
      logger_(std::move(logger)),
      persistence_max_retries_(persistence_max_retries),
      persistence_queue_limit_(std::max<std::size_t>(1, persistence_queue_limit)),
      persistence_batch_size_(std::max<std::size_t>(1, persistence_batch_size)),
      persistence_worker_([this](std::stop_token stop_token) { persistence_loop(stop_token); }) {}

ChatService::~ChatService() {
    // 必须在条件变量和队列仍然存活时显式停止并等待 worker，
    // 确保析构阶段不会出现后台线程访问已销毁成员。
    persistence_worker_.request_stop();
    persistence_condition_.notify_all();
    if (persistence_worker_.joinable()) {
        persistence_worker_.join();
    }
}

std::size_t ChatService::subscribe(mf::game::ChatChannel channel, DeliveryFn delivery) {
    std::scoped_lock lock{mutex_};
    // 用订阅 id 作为解绑句柄，
    // 可以让 websocket session 干净地退订，而不把内部容器细节暴露出去。
    const auto id = next_subscription_id_++;
    auto& channel_subscriptions = subscriptions_[channel.channel_id];
    channel_subscriptions.subscriptions.push_back(Subscription{
        .id = id,
        .channel = std::move(channel),
        .delivery = std::move(delivery),
    });
    auto snapshot = std::make_shared<std::vector<DeliveryFn>>();
    snapshot->reserve(channel_subscriptions.subscriptions.size());
    for (const auto& subscription : channel_subscriptions.subscriptions) {
        snapshot->push_back(subscription.delivery);
    }
    channel_subscriptions.delivery_snapshot = std::move(snapshot);
    return id;
}

void ChatService::unsubscribe(const std::string& channel_id, std::size_t subscription_id) {
    std::scoped_lock lock{mutex_};
    if (const auto it = subscriptions_.find(channel_id); it != subscriptions_.end()) {
        auto& channel_subscriptions = it->second;
        auto& subscriptions = channel_subscriptions.subscriptions;
        subscriptions.erase(
            std::remove_if(
                subscriptions.begin(),
                subscriptions.end(),
                [subscription_id](const Subscription& subscription) {
                    return subscription.id == subscription_id;
                }),
            subscriptions.end());

        if (subscriptions.empty()) {
            subscriptions_.erase(it);
            return;
        }

        auto snapshot = std::make_shared<std::vector<DeliveryFn>>();
        snapshot->reserve(subscriptions.size());
        for (const auto& subscription : subscriptions) {
            snapshot->push_back(subscription.delivery);
        }
        channel_subscriptions.delivery_snapshot = std::move(snapshot);
    }
}

ChatService::PublishResult ChatService::publish(mf::game::ChatMessage message) {
    if (const auto validation = validate_publish(message); !validation.ok) {
        return validation;
    }

    // 同频道使用同一把分片锁，保证消息落库顺序与广播顺序一致。
    // 锁不与订阅表 mutex_ 共用；数据库等待期间，其他频道仍可以继续处理。
    std::scoped_lock publish_lock{publish_mutex_for(message.channel.channel_id)};
    if (message.sent_at.time_since_epoch().count() == 0) {
        message.sent_at = std::chrono::system_clock::now();
    }
    // 消息 ID 必须在广播前确定，避免客户端收到的对象和异步落库对象不一致。
    if (message.message_id.empty()) {
        static std::atomic<std::uint64_t> message_sequence = 1;
        message.message_id = "msg_" + std::to_string(
            std::chrono::duration_cast<std::chrono::microseconds>(
                message.sent_at.time_since_epoch()).count()) +
            "_" + std::to_string(message_sequence.fetch_add(1, std::memory_order_relaxed));
    }
    if (!enqueue_persistence(message)) {
        return PublishResult{
            .ok = false,
            .code = repository_->supports_outbox()
                ? "chat_persistence_unavailable"
                : "chat_persistence_queue_full",
            .message = repository_->supports_outbox()
                ? "聊天持久化暂不可用，请稍后重试"
                : "聊天持久化队列已满，请稍后重试",
        };
    }
    state_service_->on_message_persisted(message);
    broadcast(message);
    return PublishResult{
        .ok = true,
        .delivered_message = message,
    };
}

bool ChatService::enqueue_persistence(mf::game::ChatMessage message) {
    // MySQL/Redis 模式先写持久化 outbox，再向客户端广播；
    // 进程崩溃后 worker 可以从数据库恢复未完成消息。
    if (repository_->supports_outbox()) {
        try {
            repository_->append_to_outbox(std::move(message));
            {
                std::scoped_lock lock{persistence_mutex_};
                persistence_wakeup_ = true;
            }
            persistence_condition_.notify_one();
            return true;
        } catch (const std::exception& error) {
            if (metrics_) {
                metrics_->increment_counter("mf_chat_persistence_outbox_enqueue_failed_total");
            }
            if (logger_) {
                logger_->log(
                    mf::core::LogLevel::error,
                    "chat outbox enqueue failed error=" + std::string{error.what()});
            }
            return false;
        } catch (...) {
            if (metrics_) {
                metrics_->increment_counter("mf_chat_persistence_outbox_enqueue_failed_total");
            }
            if (logger_) {
                logger_->log(mf::core::LogLevel::error, "chat outbox enqueue failed with unknown exception");
            }
            return false;
        }
    }

    std::scoped_lock lock{persistence_mutex_};
    if (persistence_queue_.size() + persistence_in_flight_ >= persistence_queue_limit_) {
        if (metrics_) {
            metrics_->increment_counter("mf_chat_persistence_rejected_total");
        }
        return false;
    }
    persistence_queue_.push_back(std::move(message));
    if (metrics_) {
        metrics_->set_gauge(
            "mf_chat_persistence_queue_pending",
            static_cast<double>(persistence_queue_.size() + persistence_in_flight_));
    }
    persistence_condition_.notify_one();
    return true;
}

void ChatService::persistence_loop(std::stop_token stop_token) {
    while (true) {
        std::vector<mf::game::ChatMessage> batch;
        {
            std::unique_lock lock{persistence_mutex_};
            persistence_condition_.wait_for(
                lock,
                stop_token,
                std::chrono::milliseconds{1000},
                [this] { return !persistence_queue_.empty() || persistence_wakeup_; });
            persistence_wakeup_ = false;
            while (!persistence_queue_.empty() && batch.size() < persistence_batch_size_) {
                batch.push_back(std::move(persistence_queue_.front()));
                persistence_queue_.pop_front();
            }
            // 第一条消息到达后给同一波流量一个极短聚合窗口，
            // 避免每次 notify 都只提交一条；满批次时立即结束等待。
            if (batch.size() < persistence_batch_size_ && !stop_token.stop_requested()) {
                persistence_condition_.wait_for(
                    lock,
                    stop_token,
                    std::chrono::milliseconds{2},
                    [this] { return persistence_queue_.size() >= persistence_batch_size_; });
                while (!persistence_queue_.empty() && batch.size() < persistence_batch_size_) {
                    batch.push_back(std::move(persistence_queue_.front()));
                    persistence_queue_.pop_front();
                }
            }
            if (metrics_) {
                metrics_->set_gauge(
                    "mf_chat_persistence_queue_pending",
                    static_cast<double>(persistence_queue_.size() + persistence_in_flight_));
            }
            if (batch.empty() && stop_token.stop_requested() && !repository_->supports_outbox()) {
                return;
            }
            persistence_in_flight_ = batch.size();
            if (metrics_) {
                metrics_->set_gauge(
                    "mf_chat_persistence_queue_pending",
                    static_cast<double>(persistence_queue_.size() + persistence_in_flight_));
            }
        }
        if (!batch.empty()) {
            try {
                persist_batch(std::move(batch));
            } catch (...) {
                // 最外层线程边界兜底，避免自定义 Logger/metrics 实现抛异常时终止 worker。
                try {
                    if (logger_) {
                        logger_->log(
                            mf::core::LogLevel::error,
                            "chat persistence worker caught an unexpected exception");
                    }
                } catch (...) {
                }
            }
        } else if (repository_->supports_outbox()) {
            try {
                const auto outbox = repository_->drain_outbox_batch(persistence_batch_size_);
                if (metrics_) {
                    metrics_->set_gauge(
                        "mf_chat_persistence_queue_pending",
                        static_cast<double>(outbox.pending_count));
                    metrics_->set_gauge(
                        "mf_chat_persistence_outbox_failed",
                        static_cast<double>(outbox.failed_count));
                    metrics_->set_gauge(
                        "mf_chat_persistence_outbox_oldest_pending_age_seconds",
                        static_cast<double>(outbox.oldest_pending_age_seconds));
                    if (!outbox.messages.empty()) {
                        metrics_->increment_counter(
                            "mf_chat_persistence_outbox_drained_total",
                            static_cast<double>(outbox.messages.size()));
                    }
                }
                if (outbox.messages.size() >= persistence_batch_size_) {
                    {
                        std::scoped_lock lock{persistence_mutex_};
                        persistence_wakeup_ = true;
                    }
                    persistence_condition_.notify_one();
                }
                if (outbox.messages.empty() && stop_token.stop_requested()) {
                    return;
                }
            } catch (const std::exception& error) {
                if (metrics_) {
                    metrics_->increment_counter("mf_chat_persistence_outbox_retry_total");
                }
                if (logger_) {
                    logger_->log(
                        mf::core::LogLevel::warn,
                        "chat outbox drain failed; will retry error=" + std::string{error.what()});
                }
                if (stop_token.stop_requested()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
            } catch (...) {
                if (metrics_) {
                    metrics_->increment_counter("mf_chat_persistence_outbox_retry_total");
                }
                if (logger_) {
                    logger_->log(mf::core::LogLevel::warn, "chat outbox drain failed with unknown exception");
                }
                if (stop_token.stop_requested()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
            }
        }
        {
            std::scoped_lock lock{persistence_mutex_};
            persistence_in_flight_ = 0;
            if (metrics_) {
                metrics_->set_gauge(
                    "mf_chat_persistence_queue_pending",
                    static_cast<double>(persistence_queue_.size()));
            }
        }
    }
}

void ChatService::persist_batch(std::vector<mf::game::ChatMessage> batch) {
    if (batch.empty()) {
        return;
    }
    const auto batch_size = batch.size();
    // 退避时间有上限，避免数据库故障时 worker 高频重试或停机无限等待。
    constexpr std::array retry_delays{
        std::chrono::milliseconds{100},
        std::chrono::milliseconds{250},
        std::chrono::milliseconds{500},
        std::chrono::milliseconds{1000},
        std::chrono::milliseconds{2000},
    };
    for (std::size_t attempt = 0;; ++attempt) {
        try {
            repository_->append_batch(batch);
            if (metrics_) {
                metrics_->increment_counter(
                    "mf_chat_persistence_batches_total", 1.0, {{"status", "ok"}});
                metrics_->increment_counter(
                    "mf_chat_persistence_messages_total",
                    static_cast<double>(batch_size),
                    {{"status", "ok"}});
            }
            return;
        } catch (const std::exception& error) {
            if (attempt >= persistence_max_retries_) {
                if (metrics_) {
                    metrics_->increment_counter(
                        "mf_chat_persistence_batches_total", 1.0, {{"status", "failed"}});
                    metrics_->increment_counter(
                        "mf_chat_persistence_messages_total",
                        static_cast<double>(batch_size),
                        {{"status", "failed"}});
                    metrics_->increment_counter("mf_chat_persistence_failed_after_retries_total", batch_size);
                }
                if (logger_) {
                    logger_->log(
                        mf::core::LogLevel::error,
                        "chat persistence batch abandoned after retries; messages=" +
                            std::to_string(batch_size) + " error=" + error.what());
                }
                return;
            }

            if (metrics_) {
                metrics_->increment_counter("mf_chat_persistence_retry_total");
            }
            if (logger_) {
                logger_->log(
                    mf::core::LogLevel::warn,
                    "chat persistence batch retry=" + std::to_string(attempt + 1) +
                        " messages=" + std::to_string(batch_size) + " error=" + error.what());
            }
            std::this_thread::sleep_for(retry_delays[std::min(attempt, retry_delays.size() - 1)]);
        } catch (...) {
            if (metrics_) {
                metrics_->increment_counter("mf_chat_persistence_unknown_error_total");
            }
            if (logger_) {
                logger_->log(mf::core::LogLevel::error, "chat persistence batch failed with unknown exception");
            }
            return;
        }
    }
}

std::mutex& ChatService::publish_mutex_for(std::string_view channel_id) {
    // 频道 id 是服务端生成/校验过的字符串；FNV-1a 足够用于本地分片，
    // 不要求跨进程保持相同结果，只要求相同频道落到同一把锁。
    std::size_t hash = 1469598103934665603ULL;
    for (const auto character : channel_id) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return publish_mutexes_[hash & (kPublishShardCount - 1)];
}

std::optional<mf::game::ChatMessage> ChatService::recall(
    const std::string& channel_id,
    const std::string& message_id,
    const std::string& operator_player_id,
    bool) {
    auto recalled = repository_->recall(channel_id, message_id, operator_player_id);
    if (recalled) {
        broadcast(*recalled);
    }
    return recalled;
}

mf::game::ChatMuteEntry ChatService::mute_player(
    const std::string& channel_id,
    const std::string& muted_player_id,
    const std::string& muted_by_player_id,
    std::string reason,
    std::optional<std::chrono::system_clock::time_point> muted_until) {
    return moderation_repository_->mute_player(
        channel_id,
        muted_player_id,
        muted_by_player_id,
        std::move(reason),
        muted_until);
}

void ChatService::clear_mute(const std::string& channel_id, const std::string& muted_player_id) {
    moderation_repository_->clear_mute(channel_id, muted_player_id);
}

std::vector<mf::game::ChatMessage> ChatService::history(const std::string& channel_id, std::size_t limit) const {
    MF_PROFILE_SCOPE("Chat history");
    // 历史消息查询故意完全委托给存储层，
    // 这样未来可以自由决定是放 SQL、Redis，还是专门的消息存储。
    return repository_->recent_messages(channel_id, limit);
}

mf::game::ChatHistoryPage ChatService::history_page(
    const std::string& channel_id,
    std::string_view cursor,
    std::size_t limit) const {
    MF_PROFILE_SCOPE("Chat history page");
    return repository_->history_page(channel_id, cursor, limit);
}

std::vector<mf::game::ChatMessage> ChatService::latest_messages(
    const std::vector<std::string>& channel_ids,
    std::size_t limit) const {
    MF_PROFILE_SCOPE("Chat latest messages batch");
    return repository_->latest_messages(channel_ids, limit);
}

void ChatService::broadcast(const mf::game::ChatMessage& message) {
    std::shared_ptr<const std::vector<DeliveryFn>> deliveries;
    {
        std::scoped_lock lock{mutex_};
        if (const auto it = subscriptions_.find(message.channel.channel_id); it != subscriptions_.end()) {
            deliveries = it->second.delivery_snapshot;
        }
    }

    if (!deliveries) {
        return;
    }

    for (const auto& delivery : *deliveries) {
        delivery(message);
    }
}

ChatService::PublishResult ChatService::validate_publish(const mf::game::ChatMessage& message) const {
    if (message.kind == mf::game::ChatMessageKind::system) {
        return PublishResult{.ok = true};
    }

    if (message.channel.kind == mf::game::ChatChannelKind::friend_direct) {
        std::stringstream input(message.channel.scope_id);
        std::string token;
        std::vector<std::string> participants;
        while (std::getline(input, token, ':')) {
            if (!token.empty()) {
                participants.push_back(token);
            }
        }
        const auto target_player_id =
            participants.size() == 2
                ? (participants[0] == message.sender_player_id ? participants[1] : participants[0])
                : std::string{};

        if (target_player_id.empty()) {
            return PublishResult{
                .ok = false,
                .code = "invalid_friend_channel",
                .message = "好友私聊频道标识不合法",
            };
        }

        if (friend_repository_->is_chat_blocked(message.sender_player_id, target_player_id) ||
            friend_repository_->is_chat_blocked(target_player_id, message.sender_player_id)) {
            return PublishResult{
                .ok = false,
                .code = "chat_blocked",
                .message = "好友私聊已被屏蔽",
            };
        }

        const auto links = friend_repository_->list_friends(message.sender_player_id);
        const auto is_friend = std::any_of(
            links.begin(),
            links.end(),
            [&](const mf::game::FriendLink& link) {
                return link.friend_player_id == target_player_id;
            });
        if (!is_friend) {
            return PublishResult{
                .ok = false,
                .code = "friend_required",
                .message = "好友私聊要求双方已经建立好友关系",
            };
        }
    }

    if (message.channel.kind != mf::game::ChatChannelKind::friend_direct) {
        if (const auto mute = moderation_repository_->find_active_mute(
                message.channel.channel_id,
                message.sender_player_id)) {
            return PublishResult{
                .ok = false,
                .code = "chat_muted",
                .message = "玩家已在该频道被禁言",
            };
        }
    }

    return PublishResult{.ok = true};
}

}  // namespace mf::chat 命名空间
