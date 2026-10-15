#pragma once

// 文件职责：
// 定义聊天状态服务，负责在线状态和频道成员关系。
// 调用流程：
// ChatGatewaySession 在 auth/join/cleanup 边界调用 mark_online()/join_channel()/leave_channel()/mark_offline() ->
// HTTP 观察接口调用 is_online()/list_channel_members() ->
// 底层可以切换为内存版或 Redis 版实现，而不影响上层协议流程。

#include "mf/game/models.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mf::chat {

// 频道成员分页结果。
// members 为当前页成员列表；total_count 为频道成员总数；has_more/next_cursor 用于继续翻页。
struct ChannelMembersPage {
    std::vector<std::string> members;
    std::int64_t total_count = 0;
    std::string next_cursor;
    bool has_more = false;
};

// 好友页所需的在线状态和未读数；Redis 实现可在同一次 pipeline 中批量返回两者。
struct FriendOverviewState {
    std::vector<mf::game::FriendPresence> presence;
    std::vector<mf::game::ChannelUnreadCount> unread_counts;
};

class ChatStateService {
public:
    virtual ~ChatStateService() = default;

    // 返回当前同一玩家仍然活跃的连接数。
    virtual std::size_t mark_online(const std::string& player_id) = 0;
    virtual std::size_t mark_offline(const std::string& player_id) = 0;

    // 返回当前同一玩家在同一频道上的活跃会话数。
    virtual std::size_t join_channel(const mf::game::ChatChannel& channel, const std::string& player_id) = 0;
    virtual std::size_t leave_channel(const std::string& channel_id, const std::string& player_id) = 0;

    // 第一版未读/离线消息聚焦好友私聊。
    // 频道消息真正变成“已读”需要客户端显式上报，因此这里单独暴露 mark_channel_read。
    virtual void on_message_persisted(const mf::game::ChatMessage& message) = 0;
    virtual void mark_channel_read(const std::string& player_id, const std::string& channel_id) = 0;
    virtual std::vector<mf::game::ChannelUnreadCount> list_unread_counts(const std::string& player_id) const = 0;
    // 好友列表只需要当前页频道的未读数，避免读取该玩家所有频道的未读 Hash。
    virtual std::vector<mf::game::ChannelUnreadCount> list_unread_counts_for_channels(
        const std::string& player_id,
        const std::vector<std::string>& channel_ids) const = 0;
    // 消费离线通知但不改变未读数；打开频道或显式 mark_channel_read 才确认已读。
    // 不提供客户端 ACK，响应丢失时不保证消息重投。
    virtual std::vector<mf::game::ChatMessage> pull_offline_messages(
        const std::string& player_id,
        std::size_t limit) = 0;

    virtual bool is_online(const std::string& player_id) const = 0;
    virtual std::vector<mf::game::FriendPresence> list_friend_presence(
        const std::vector<std::string>& player_ids) const = 0;

    // 默认兼容实现由两个既有方法组合；Redis 可覆盖为单次网络往返。
    virtual FriendOverviewState list_friend_overview_state(
        const std::string& player_id,
        const std::vector<std::string>& friend_player_ids,
        const std::vector<std::string>& channel_ids) const {
        return FriendOverviewState{
            .presence = list_friend_presence(friend_player_ids),
            .unread_counts = list_unread_counts_for_channels(player_id, channel_ids),
        };
    }

    // 分页读取频道成员。total_count 通过 O(1) 的 SCARD/尺寸独立获取，
    // 避免为了显示成员数而全量拉取成员列表。
    // cursor 为不透明游标：首屏传空串，后续透传上一页返回的 next_cursor。
    // limit 会被实现夹在 [1, 1000] 之间。
    virtual ChannelMembersPage list_channel_members(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const = 0;
};

// shard_count 会被向上取整到 2 的幂；传入 0 等价于默认 64。
// 每个 shard 持有独立 shared_mutex，值越大锁竞争越小，但内存与 fd 开销也越大。
std::shared_ptr<ChatStateService> make_in_memory_chat_state_service(std::size_t shard_count = 64);

}  // namespace mf::chat 命名空间
