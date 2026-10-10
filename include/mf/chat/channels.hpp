#pragma once

// 文件职责：
// 定义聊天频道和消息类型的枚举、构造函数与字符串转换。
// 调用流程：
// HTTP/WS 层根据业务场景构造 ChatChannel ->
// ChatService 订阅或发布时按 channel_id 路由 ->
// JSON 层把 kind/type 转成字符串返回给客户端。

#include "mf/game/models.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mf::chat {

const char* to_string(mf::game::ChatChannelKind kind);
const char* to_string(mf::game::ChatMessageKind kind);
std::optional<mf::game::ChatChannelKind> parse_channel_kind(std::string_view value);
std::optional<mf::game::ChatMessageKind> parse_message_kind(std::string_view value);

mf::game::ChatChannel make_lobby_channel(std::string lobby_id = "global");
mf::game::ChatChannel make_room_channel(std::string room_id);
mf::game::ChatChannel make_friend_channel(std::string player_a, std::string player_b);

}  // namespace mf::chat 命名空间
