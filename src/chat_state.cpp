#include "mf/chat/state.hpp"

// 文件职责：
// 提供聊天状态服务的内存版实现，负责在线状态和频道成员关系。
// 调用流程：
// in_memory 模式下，ChatGateway 在 auth/join/cleanup 时调用本实现 ->
// 本实现维护玩家在线连接计数和频道成员计数 ->
// HTTP 观察接口直接读取这些内存状态。
//
// 并发策略：
// 把状态按 key 分片成可配置数量的 shard，避免全局一把锁成为热点。
// 每个 shard 持有独立的 shared_mutex，读操作用 shared_lock，写操作用 unique_lock。
// 涉及多个 shard 的操作按 shard 序号升序加锁；同时涉及玩家态与频道态两类 shard 的
// 操作（join_channel）先锁玩家 shard、再锁频道 shard，保证锁获取遵循全局顺序、避免死锁。

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mf::chat {
namespace {

constexpr std::size_t kDefaultShardCount = 64;
constexpr std::size_t kMaxShardCount = 4096;

std::size_t next_power_of_two(std::size_t value) {
    std::size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// 使用 FNV-1a + splitmix64 最终化，避免 std::hash 面对恶意 key 时被定向打到同一分片，
// 同时提供稳定、均匀的扩散。内存实现无需跨进程一致，这里只要求同 key 同 shard、分布均匀。
std::size_t state_key_hash(std::string_view key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return static_cast<std::size_t>(hash);
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

// 玩家维度的状态：在线计数、未读计数、离线消息，统一按 player_id 分片。
struct PlayerStateShard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::size_t> online_counts;
    std::unordered_map<std::string, std::unordered_map<std::string, std::int64_t>> unread_counts;
    std::unordered_map<std::string, std::vector<mf::game::ChatMessage>> offline_messages;
};

// 频道维度的状态：频道成员计数，按 channel_id 分片。
struct ChannelStateShard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> member_counts;
};

class InMemoryChatStateService final : public ChatStateService {
public:
    explicit InMemoryChatStateService(std::size_t shard_count)
        : shard_count_(next_power_of_two(std::clamp<std::size_t>(
              shard_count == 0 ? kDefaultShardCount : shard_count, 1, kMaxShardCount))),
          player_shards_(shard_count_),
          channel_shards_(shard_count_) {}

    std::size_t mark_online(const std::string& player_id) override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::unique_lock lock{shard.mutex};
        return ++shard.online_counts[player_id];
    }

    std::size_t mark_offline(const std::string& player_id) override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::unique_lock lock{shard.mutex};
        const auto it = shard.online_counts.find(player_id);
        if (it == shard.online_counts.end()) {
            return 0;
        }
        if (it->second <= 1) {
            shard.online_counts.erase(it);
            return 0;
        }
        --it->second;
        return it->second;
    }

    std::size_t join_channel(const mf::game::ChatChannel& channel, const std::string& player_id) override {
        // 全局加锁顺序：所有玩家 shard 都先于所有频道 shard，避免跨 shard 操作构成等待环。
        auto& player_shard = player_shards_[shard_index(player_id)];
        auto& channel_shard = channel_shards_[shard_index(channel.channel_id)];
        std::unique_lock player_lock{player_shard.mutex};
        std::unique_lock channel_lock{channel_shard.mutex};
        player_shard.unread_counts[player_id].erase(channel.channel_id);
        auto& members = channel_shard.member_counts[channel.channel_id];
        return ++members[player_id];
    }

    std::size_t leave_channel(const std::string& channel_id, const std::string& player_id) override {
        auto& shard = channel_shards_[shard_index(channel_id)];
        std::unique_lock lock{shard.mutex};
        const auto channel_it = shard.member_counts.find(channel_id);
        if (channel_it == shard.member_counts.end()) {
            return 0;
        }

        auto& members = channel_it->second;
        const auto member_it = members.find(player_id);
        if (member_it == members.end()) {
            return 0;
        }

        if (member_it->second <= 1) {
            members.erase(member_it);
            if (members.empty()) {
                shard.member_counts.erase(channel_it);
            }
            return 0;
        }

        --member_it->second;
        return member_it->second;
    }

    void on_message_persisted(const mf::game::ChatMessage& message) override {
        if (message.channel.kind != mf::game::ChatChannelKind::friend_direct) {
            return;
        }

        std::vector<std::string> recipients;
        for (const auto& player_id : friend_channel_participants(message.channel)) {
            if (player_id == message.sender_player_id) {
                continue;
            }
            recipients.push_back(player_id);
        }
        if (recipients.empty()) {
            return;
        }

        // 按 shard 序号升序处理，同一 shard 内连续更新，只加一次写锁。
        std::sort(recipients.begin(), recipients.end(), [&](const std::string& left, const std::string& right) {
            return shard_index(left) < shard_index(right);
        });
        std::size_t current_shard = shard_index(recipients.front());
        std::unique_lock lock{player_shards_[current_shard].mutex};
        for (const auto& player_id : recipients) {
            const auto shard_idx = shard_index(player_id);
            if (shard_idx != current_shard) {
                lock.unlock();
                current_shard = shard_idx;
                lock = std::unique_lock{player_shards_[current_shard].mutex};
            }
            auto& shard = player_shards_[current_shard];
            ++shard.unread_counts[player_id][message.channel.channel_id];
            if (!shard.online_counts.contains(player_id)) {
                shard.offline_messages[player_id].push_back(message);
            }
        }
    }

    void mark_channel_read(const std::string& player_id, const std::string& channel_id) override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::unique_lock lock{shard.mutex};
        shard.unread_counts[player_id].erase(channel_id);
    }

    std::vector<mf::game::ChannelUnreadCount> list_unread_counts(const std::string& player_id) const override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::shared_lock lock{shard.mutex};
        std::vector<mf::game::ChannelUnreadCount> items;
        if (const auto it = shard.unread_counts.find(player_id); it != shard.unread_counts.end()) {
            items.reserve(it->second.size());
            for (const auto& [channel_id, unread_count] : it->second) {
                items.push_back(mf::game::ChannelUnreadCount{
                    .channel_id = channel_id,
                    .unread_count = unread_count,
                });
            }
        }
        return items;
    }

    // 只按当前好友页的频道 ID 查 Hash，避免遍历玩家所有未读频道。
    std::vector<mf::game::ChannelUnreadCount> list_unread_counts_for_channels(
        const std::string& player_id,
        const std::vector<std::string>& channel_ids) const override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::shared_lock lock{shard.mutex};
        std::vector<mf::game::ChannelUnreadCount> items;
        const auto player_it = shard.unread_counts.find(player_id);
        if (player_it == shard.unread_counts.end()) {
            return items;
        }

        items.reserve(channel_ids.size());
        for (const auto& channel_id : channel_ids) {
            if (const auto unread_it = player_it->second.find(channel_id); unread_it != player_it->second.end()) {
                items.push_back(mf::game::ChannelUnreadCount{
                    .channel_id = channel_id,
                    .unread_count = unread_it->second,
                });
            }
        }
        return items;
    }

    std::vector<mf::game::ChatMessage> pull_offline_messages(
        const std::string& player_id,
        std::size_t limit) override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::unique_lock lock{shard.mutex};
        std::vector<mf::game::ChatMessage> result;
        auto& messages = shard.offline_messages[player_id];
        if (messages.empty()) {
            return result;
        }

        const auto count = std::min<std::size_t>(limit, messages.size());
        result.assign(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(count));
        messages.erase(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(count));
        if (messages.empty()) {
            shard.offline_messages.erase(player_id);
        }

        for (const auto& message : result) {
            shard.unread_counts[player_id].erase(message.channel.channel_id);
        }
        return result;
    }

    bool is_online(const std::string& player_id) const override {
        auto& shard = player_shards_[shard_index(player_id)];
        std::shared_lock lock{shard.mutex};
        return shard.online_counts.contains(player_id);
    }

    std::vector<mf::game::FriendPresence> list_friend_presence(
        const std::vector<std::string>& player_ids) const override {
        std::vector<mf::game::FriendPresence> items(player_ids.size());

        // 记录 (shard 序号, 输入下标)，按 shard 升序后逐 shard 加读锁，保证结果顺序不变。
        std::vector<std::pair<std::size_t, std::size_t>> ordered;
        ordered.reserve(player_ids.size());
        for (std::size_t i = 0; i < player_ids.size(); ++i) {
            ordered.emplace_back(shard_index(player_ids[i]), i);
        }
        std::sort(ordered.begin(), ordered.end());

        for (std::size_t i = 0; i < ordered.size();) {
            const auto shard_idx = ordered[i].first;
            auto& shard = player_shards_[shard_idx];
            std::shared_lock lock{shard.mutex};
            for (; i < ordered.size() && ordered[i].first == shard_idx; ++i) {
                const auto& player_id = player_ids[ordered[i].second];
                const auto it = shard.online_counts.find(player_id);
                auto& item = items[ordered[i].second];
                item.player_id = player_id;
                item.online = it != shard.online_counts.end();
                item.connection_count = it != shard.online_counts.end() ? static_cast<std::int64_t>(it->second) : 0;
            }
        }
        return items;
    }

    ChannelMembersPage list_channel_members(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const override {
        auto& shard = channel_shards_[shard_index(channel_id)];
        std::shared_lock lock{shard.mutex};
        ChannelMembersPage page;
        if (const auto it = shard.member_counts.find(channel_id); it != shard.member_counts.end()) {
            page.total_count = static_cast<std::int64_t>(it->second.size());

            // 内存版用十进制 offset 作为不透明游标，与 Redis 版的 SSCAN 游标语义解耦。
            std::size_t offset = 0;
            if (!cursor.empty()) {
                try {
                    offset = static_cast<std::size_t>(std::stoull(std::string{cursor}));
                } catch (...) {
                    offset = 0;
                }
            }
            offset = std::min<std::size_t>(offset, it->second.size());

            const auto bounded_limit = std::clamp<std::size_t>(limit, 1, 1000);
            page.members.reserve(std::min<std::size_t>(bounded_limit, it->second.size() - offset));
            auto member_it = it->second.begin();
            for (std::size_t i = 0; i < offset && member_it != it->second.end(); ++i, ++member_it) {
            }
            for (std::size_t i = 0; i < bounded_limit && member_it != it->second.end(); ++i, ++member_it) {
                page.members.push_back(member_it->first);
            }
            page.has_more = offset + page.members.size() < it->second.size();
            if (page.has_more) {
                page.next_cursor = std::to_string(offset + page.members.size());
            }
        }
        return page;
    }

private:
    std::size_t shard_index(std::string_view key) const {
        return state_key_hash(key) & (shard_count_ - 1);
    }

    std::size_t shard_count_;
    std::vector<PlayerStateShard> player_shards_;
    std::vector<ChannelStateShard> channel_shards_;
};

}  // namespace

std::shared_ptr<ChatStateService> make_in_memory_chat_state_service(std::size_t shard_count) {
    return std::make_shared<InMemoryChatStateService>(shard_count);
}

}  // namespace mf::chat
