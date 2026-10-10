#pragma once

// 文件职责：
// 定义游戏领域模型到 JSON 的映射函数。
// 调用流程：
// handler/chat gateway 拿到 Account、PlayerProfile、ChatMessage 等对象 ->
// 调用 to_json()/to_json_array() -> 拼进 HTTP 或 WebSocket 响应里返回给客户端。

#include "mf/game/models.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value_to.hpp>

namespace mf::game {

// 这些辅助函数把序列化策略集中在一个地方，
// 避免 handler 和聊天代码重复写字段映射逻辑。
boost::json::object to_json(const Account& account);
boost::json::object to_json(const PasswordResetTicket& ticket);
boost::json::object to_json(const CaptchaChallenge& challenge);
boost::json::object to_json(const BlacklistEntry& entry);
boost::json::object to_json(const PlayerProfile& profile);
boost::json::object to_json(const CurrencyBalance& currency);
boost::json::object to_json(const InventoryItem& item);
boost::json::object to_json(const ItemDefinition& item);
boost::json::object to_json(const RewardEntry& reward);
boost::json::object to_json(const RewardTemplate& reward_template);
boost::json::object to_json(const GachaPool& pool);
boost::json::object to_json(const GachaPoolItem& item);
boost::json::object to_json(const GachaPlayerState& state);
boost::json::object to_json(const GachaHistoryEntry& entry);
boost::json::object to_json(const GachaDrawResult& result);
boost::json::object to_json(const ShopProduct& product);
boost::json::object to_json(const ShopPurchaseRecord& purchase);
boost::json::object to_json(const ShopPurchaseResult& result);
boost::json::object to_json(const LootDrop& drop);
boost::json::object to_json(const LootPickupResult& result);
boost::json::object to_json(const QuestObjectiveDefinition& objective);
boost::json::object to_json(const QuestDefinition& quest);
boost::json::object to_json(const PlayerQuestObjective& objective);
boost::json::object to_json(const PlayerQuest& quest);
boost::json::object to_json(const QuestSnapshot& snapshot);
boost::json::object to_json(const QuestEventResult& result);
boost::json::object to_json(const QuestClaimResult& result);
boost::json::object to_json(const AnnouncementEntry& announcement);
boost::json::object to_json(const RewardGrantRecord& grant);
boost::json::object to_json(const AuditLogEntry& audit);
boost::json::object to_json(const AuditLogWriteRequest& audit_request);
boost::json::object to_json(const AdminPrincipal& admin);
boost::json::object to_json(const AdminTask& task);
boost::json::object to_json(const MailMessage& mail);
boost::json::object to_json(const FriendLink& link);
boost::json::object to_json(const FriendRequest& request);
boost::json::object to_json(const ChatBlockEntry& block);
boost::json::object to_json(const ChatMuteEntry& mute);
boost::json::object to_json(const ChannelUnreadCount& unread);
boost::json::object to_json(const FriendPresence& presence);
boost::json::object to_json(const Session& session);
boost::json::object to_json(const ChatChannel& channel);
boost::json::object to_json(const ChatMessage& message);
boost::json::array to_json_array(const std::vector<std::string>& values);
boost::json::array to_json_array(const std::vector<PlayerProfile>& values);
boost::json::array to_json_array(const std::vector<CurrencyBalance>& values);
boost::json::array to_json_array(const std::vector<InventoryItem>& values);
boost::json::array to_json_array(const std::vector<ItemDefinition>& values);
boost::json::array to_json_array(const std::vector<RewardEntry>& values);
boost::json::array to_json_array(const std::vector<RewardTemplate>& values);
boost::json::array to_json_array(const std::vector<GachaPool>& values);
boost::json::array to_json_array(const std::vector<GachaPoolItem>& values);
boost::json::array to_json_array(const std::vector<GachaHistoryEntry>& values);
boost::json::array to_json_array(const std::vector<ShopProduct>& values);
boost::json::array to_json_array(const std::vector<ShopPurchaseRecord>& values);
boost::json::array to_json_array(const std::vector<LootDrop>& values);
boost::json::array to_json_array(const std::vector<QuestObjectiveDefinition>& values);
boost::json::array to_json_array(const std::vector<QuestDefinition>& values);
boost::json::array to_json_array(const std::vector<PlayerQuestObjective>& values);
boost::json::array to_json_array(const std::vector<PlayerQuest>& values);
boost::json::array to_json_array(const std::vector<AnnouncementEntry>& values);
boost::json::array to_json_array(const std::vector<RewardGrantRecord>& values);
boost::json::array to_json_array(const std::vector<AuditLogEntry>& values);
boost::json::array to_json_array(const std::vector<AdminPrincipal>& values);
boost::json::array to_json_array(const std::vector<AdminTask>& values);
boost::json::array to_json_array(const std::vector<MailMessage>& values);
boost::json::array to_json_array(const std::vector<FriendLink>& values);
boost::json::array to_json_array(const std::vector<FriendRequest>& values);
boost::json::array to_json_array(const std::vector<ChatBlockEntry>& values);
boost::json::array to_json_array(const std::vector<BlacklistEntry>& values);
boost::json::array to_json_array(const std::vector<ChatMuteEntry>& values);
boost::json::array to_json_array(const std::vector<ChannelUnreadCount>& values);
boost::json::array to_json_array(const std::vector<FriendPresence>& values);
boost::json::array to_json_array(const std::vector<ChatMessage>& values);

}  // namespace mf::game 命名空间
