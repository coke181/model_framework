#include "mf/chat/channels.hpp"

// 文件职责：
// 实现聊天频道模型和消息类型的转换、构造逻辑。
// 调用流程：
// HTTP/WS 层根据 kind 和 scope_id 构造 ChatChannel ->
// ChatService 使用 channel_id 作为路由键 ->
// JSON 层再把 kind/type 转回字符串发给客户端。

#include <algorithm>

namespace mf::chat {
namespace {

std::string canonical_friend_scope(std::string player_a, std::string player_b) {
    if (player_b < player_a) {
        std::swap(player_a, player_b);
    }
    return player_a + ":" + player_b;
}

}  // 匿名命名空间

const char* to_string(mf::game::ChatChannelKind kind) {
    switch (kind) {
    case mf::game::ChatChannelKind::lobby:
        return "lobby";
    case mf::game::ChatChannelKind::room:
        return "room";
    case mf::game::ChatChannelKind::friend_direct:
        return "friend_direct";
    }
    return "unknown";
}

const char* to_string(mf::game::ChatMessageKind kind) {
    switch (kind) {
    case mf::game::ChatMessageKind::text:
        return "text";
    case mf::game::ChatMessageKind::system:
        return "system";
    }
    return "unknown";
}

std::optional<mf::game::ChatChannelKind> parse_channel_kind(std::string_view value) {
    if (value == "lobby") {
        return mf::game::ChatChannelKind::lobby;
    }
    if (value == "room") {
        return mf::game::ChatChannelKind::room;
    }
    if (value == "friend_direct") {
        return mf::game::ChatChannelKind::friend_direct;
    }
    return std::nullopt;
}

std::optional<mf::game::ChatMessageKind> parse_message_kind(std::string_view value) {
    if (value == "text") {
        return mf::game::ChatMessageKind::text;
    }
    if (value == "system") {
        return mf::game::ChatMessageKind::system;
    }
    return std::nullopt;
}

mf::game::ChatChannel make_lobby_channel(std::string lobby_id) {
    return mf::game::ChatChannel{
        .channel_id = "lobby:" + lobby_id,
        .kind = mf::game::ChatChannelKind::lobby,
        .scope_id = std::move(lobby_id),
        .display_name = "lobby",
    };
}

mf::game::ChatChannel make_room_channel(std::string room_id) {
    return mf::game::ChatChannel{
        .channel_id = "room:" + room_id,
        .kind = mf::game::ChatChannelKind::room,
        .scope_id = std::move(room_id),
        .display_name = "room",
    };
}

mf::game::ChatChannel make_friend_channel(std::string player_a, std::string player_b) {
    const auto scope = canonical_friend_scope(std::move(player_a), std::move(player_b));
    return mf::game::ChatChannel{
        .channel_id = "friend:" + scope,
        .kind = mf::game::ChatChannelKind::friend_direct,
        .scope_id = scope,
        .display_name = "friend_direct",
    };
}

}  // namespace mf::chat 命名空间
