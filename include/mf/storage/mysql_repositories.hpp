#pragma once

// 文件职责：
// 定义基于 Boost.MySQL 的账号、玩家、好友、聊天仓储实现。
// 调用流程：
// 仓储工厂在 mysql_redis 模式下构造这些仓储 ->
// 登录、好友、聊天接口调用 AccountRepository / PlayerRepository / FriendRepository / ChatRepository ->
// 具体 SQL 读写通过 Boost.MySQL any_connection 执行。

#include "mf/game/repositories.hpp"
#include "mf/agent/repository.hpp"
#include "mf/core/logger.hpp"
#include "mf/core/metrics.hpp"
#include "mf/storage/options.hpp"
#include "mf/multiplayer/repository.hpp"
#include "mf/voice/repository.hpp"

namespace mf::storage {

void configure_mysql_observability(
    std::shared_ptr<mf::core::Logger> logger,
    std::shared_ptr<mf::core::MetricsRegistry> metrics,
    std::int64_t slow_operation_threshold_ms = 100);

std::shared_ptr<mf::game::AccountRepository> make_mysql_account_repository(const MySqlOptions& options);
std::shared_ptr<mf::agent::AgentRepository> make_mysql_agent_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::PlayerRepository> make_mysql_player_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::ItemCatalogRepository> make_mysql_item_catalog_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::GachaRepository> make_mysql_gacha_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::GachaTransactionRepository> make_mysql_gacha_transaction_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::ShopRepository> make_mysql_shop_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::LootRepository> make_mysql_loot_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::RewardTransactionRepository> make_mysql_reward_transaction_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::LootTransactionRepository> make_mysql_loot_transaction_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::ShopTransactionRepository> make_mysql_shop_transaction_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::QuestRepository> make_mysql_quest_repository(const MySqlOptions& options);
std::shared_ptr<mf::multiplayer::MultiplayerRoomRepository> make_mysql_multiplayer_room_repository(
    const MySqlOptions& options);
std::shared_ptr<mf::voice::VoiceRepository> make_mysql_voice_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::FriendRepository> make_mysql_friend_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::ChatRepository> make_mysql_chat_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::ChatModerationRepository> make_mysql_chat_moderation_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::RewardRepository> make_mysql_reward_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::RewardTemplateRepository> make_mysql_reward_template_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::AnnouncementRepository> make_mysql_announcement_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::MailRepository> make_mysql_mail_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::AuditLogRepository> make_mysql_audit_log_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::AdminRepository> make_mysql_admin_repository(const MySqlOptions& options);
std::shared_ptr<mf::game::AdminTaskRepository> make_mysql_admin_task_repository(const MySqlOptions& options);

}  // namespace mf::storage 命名空间
