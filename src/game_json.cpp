#include "mf/game/json.hpp"
#include "mf/chat/channels.hpp"

// 文件职责：
// 实现游戏领域对象到 JSON 的具体序列化逻辑。
// 调用流程：
// API 层或聊天网关拿到领域对象 ->
// 调用 to_json()/to_json_array() ->
// 把结构化数据写入 HTTP/WS 返回体。

#include <chrono>

namespace mf::game {
namespace {

std::string_view to_string(AccountAuthMode mode) {
    switch (mode) {
    case AccountAuthMode::quick:
        return "quick";
    case AccountAuthMode::password:
        return "password";
    }
    return "quick";
}

std::string_view to_string(AccountStatus status) {
    switch (status) {
    case AccountStatus::active:
        return "active";
    case AccountStatus::frozen:
        return "frozen";
    case AccountStatus::banned:
        return "banned";
    }
    return "active";
}

std::string_view to_string(FriendRequestStatus status) {
    switch (status) {
    case FriendRequestStatus::pending_incoming:
        return "pending_incoming";
    case FriendRequestStatus::pending_outgoing:
        return "pending_outgoing";
    case FriendRequestStatus::accepted:
        return "accepted";
    }
    return "pending_incoming";
}

std::string_view to_string(RewardKind kind) {
    switch (kind) {
    case RewardKind::gold:
        return "gold";
    case RewardKind::currency:
        return "currency";
    case RewardKind::item:
        return "item";
    case RewardKind::attachment_template:
        return "attachment_template";
    }
    return "gold";
}

std::string_view to_string(QuestState state) {
    switch (state) {
    case QuestState::accepted: return "accepted";
    case QuestState::completed: return "completed";
    case QuestState::claimed: return "claimed";
    }
    return "accepted";
}

std::string_view to_string(QuestClaimMode mode) {
    switch (mode) {
    case QuestClaimMode::automatic: return "auto";
    case QuestClaimMode::manual: return "manual";
    case QuestClaimMode::npc: return "npc";
    }
    return "manual";
}

std::string_view to_string(AdminTaskKind kind) {
    switch (kind) {
    case AdminTaskKind::announce:
        return "announce";
    case AdminTaskKind::batch_reward:
        return "batch_reward";
    case AdminTaskKind::batch_mail:
        return "batch_mail";
    case AdminTaskKind::batch_mute:
        return "batch_mute";
    case AdminTaskKind::batch_account_status:
        return "batch_account_status";
    }
    return "announce";
}

boost::json::value optional_unix_time(const std::optional<std::chrono::system_clock::time_point>& time_point) {
    if (!time_point) {
        return nullptr;
    }
    return std::chrono::duration_cast<std::chrono::seconds>(time_point->time_since_epoch()).count();
}

std::int64_t to_unix_seconds(std::chrono::system_clock::time_point time_point) {
    // Unix 秒时间戳对游戏客户端和运营面板都比较实用，
    // 同时也避免把时区格式化逻辑拖进后端核心层。
    return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

}  // 匿名命名空间

boost::json::object to_json(const Account& account) {
    return {
        {"account_id", account.account_id},
        {"login_name", account.login_name},
        {"display_name", account.display_name},
        {"email", account.email},
        {"phone_number", account.phone_number},
        {"platform_channel", account.platform_channel},
        {"register_source", account.register_source},
        {"auth_mode", to_string(account.auth_mode)},
        {"status", to_string(account.status)},
        {"status_reason", account.status_reason},
        {"failed_login_attempts", account.failed_login_attempts},
        {"locked_until", optional_unix_time(account.locked_until)},
        {"last_login_at", optional_unix_time(account.last_login_at)},
        {"last_login_ip", account.last_login_ip},
        {"created_at", optional_unix_time(account.created_at)},
        {"updated_at", optional_unix_time(account.updated_at)},
    };
}

boost::json::object to_json(const PasswordResetTicket& ticket) {
    return {
        {"account_id", ticket.account_id},
        {"login_name", ticket.login_name},
        {"reset_token", ticket.reset_token},
        {"expires_at", to_unix_seconds(ticket.expires_at)},
    };
}

boost::json::object to_json(const CaptchaChallenge& challenge) {
    return {
        {"challenge_id", challenge.challenge_id},
        {"prompt", challenge.prompt},
        {"expires_at", to_unix_seconds(challenge.expires_at)},
    };
}

boost::json::object to_json(const BlacklistEntry& entry) {
    return {
        {"entry_id", entry.entry_id},
        {"subject_type", entry.subject_type},
        {"subject_value", entry.subject_value},
        {"reason", entry.reason},
        {"expires_at", optional_unix_time(entry.expires_at)},
    };
}

boost::json::object to_json(const PlayerProfile& profile) {
    return {
        {"player_id", profile.player_id},
        {"account_id", profile.account_id},
        {"nickname", profile.nickname},
        {"gold", profile.gold},
        {"gem", profile.gem},
        {"currencies", to_json_array(profile.currencies)},
        {"inventory", to_json_array(profile.inventory)},
    };
}

boost::json::object to_json(const CurrencyBalance& currency) {
    return {
        {"currency_id", currency.currency_id},
        {"amount", currency.amount},
    };
}

boost::json::object to_json(const InventoryItem& item) {
    return {
        {"instance_id", item.instance_id},
        {"item_id", item.item_id},
        {"quantity", item.quantity},
    };
}

boost::json::object to_json(const ItemDefinition& item) {
    return {
        {"item_id", item.item_id},
        {"name", item.name},
        {"description", item.description},
        {"icon", item.icon},
        {"rarity", item.rarity},
        {"max_stack", item.max_stack},
        {"item_type", item.item_type},
        {"enabled", item.enabled},
        {"created_at", optional_unix_time(item.created_at)},
        {"updated_at", optional_unix_time(item.updated_at)},
    };
}

boost::json::object to_json(const RewardEntry& reward) {
    return {
        {"kind", to_string(reward.kind)},
        {"resource_id", reward.resource_id},
        {"amount", reward.amount},
    };
}

boost::json::object to_json(const RewardTemplate& reward_template) {
    return {
        {"template_id", reward_template.template_id},
        {"name", reward_template.name},
        {"rewards", to_json_array(reward_template.rewards)},
        {"created_at", optional_unix_time(reward_template.created_at)},
        {"updated_at", optional_unix_time(reward_template.updated_at)},
    };
}

boost::json::object to_json(const GachaPool& pool) {
    return {
        {"pool_id", pool.pool_id},
        {"pool_type", pool.pool_type},
        {"name", pool.name},
        {"description", pool.description},
        {"banner_icon", pool.banner_icon},
        {"cost_currency_id", pool.cost_currency_id},
        {"cost_amount_per_draw", pool.cost_amount_per_draw},
        {"pity_5", pool.pity_5},
        {"pity_4", pool.pity_4},
        {"enabled", pool.enabled},
        {"starts_at", optional_unix_time(pool.starts_at)},
        {"ends_at", optional_unix_time(pool.ends_at)},
        {"created_at", optional_unix_time(pool.created_at)},
        {"updated_at", optional_unix_time(pool.updated_at)},
    };
}

boost::json::object to_json(const GachaPoolItem& item) {
    return {
        {"pool_id", item.pool_id},
        {"item_key", item.item_key},
        {"rarity", item.rarity},
        {"weight", item.weight},
        {"is_up", item.is_up},
        {"reward", to_json(item.reward)},
    };
}

boost::json::object to_json(const GachaPlayerState& state) {
    return {
        {"player_id", state.player_id},
        {"pool_id", state.pool_id},
        {"pity_5_count", state.pity_5_count},
        {"pity_4_count", state.pity_4_count},
        {"total_draws", state.total_draws},
        {"updated_at", optional_unix_time(state.updated_at)},
    };
}

boost::json::object to_json(const GachaHistoryEntry& entry) {
    return {
        {"history_id", entry.history_id},
        {"player_id", entry.player_id},
        {"pool_id", entry.pool_id},
        {"item_key", entry.item_key},
        {"rarity", entry.rarity},
        {"is_up", entry.is_up},
        {"reward", to_json(entry.reward)},
        {"pity_5_before", entry.pity_5_before},
        {"pity_4_before", entry.pity_4_before},
        {"drawn_at", optional_unix_time(entry.drawn_at)},
    };
}

boost::json::object to_json(const GachaDrawResult& result) {
    return {
        {"pool", to_json(result.pool)},
        {"results", to_json_array(result.results)},
        {"state", to_json(result.state)},
        {"grant", result.grant ? boost::json::value{to_json(*result.grant)} : boost::json::value{}},
        {"total_cost", result.total_cost},
    };
}

boost::json::object to_json(const ShopProduct& product) {
    return {
        {"product_id", product.product_id},
        {"category", product.category},
        {"name", product.name},
        {"description", product.description},
        {"icon", product.icon},
        {"price_currency_id", product.price_currency_id},
        {"price_amount", product.price_amount},
        {"rewards", to_json_array(product.rewards)},
        {"purchase_limit", product.purchase_limit},
        {"enabled", product.enabled},
        {"starts_at", optional_unix_time(product.starts_at)},
        {"ends_at", optional_unix_time(product.ends_at)},
        {"created_at", optional_unix_time(product.created_at)},
        {"updated_at", optional_unix_time(product.updated_at)},
    };
}

boost::json::object to_json(const ShopPurchaseRecord& purchase) {
    return {
        {"purchase_id", purchase.purchase_id},
        {"player_id", purchase.player_id},
        {"product_id", purchase.product_id},
        {"price_currency_id", purchase.price_currency_id},
        {"price_amount", purchase.price_amount},
        {"rewards", to_json_array(purchase.rewards)},
        {"purchased_at", optional_unix_time(purchase.purchased_at)},
    };
}

boost::json::object to_json(const ShopPurchaseResult& result) {
    return {
        {"product", to_json(result.product)},
        {"purchase", to_json(result.purchase)},
        {"grant", result.grant ? boost::json::value{to_json(*result.grant)} : boost::json::value{}},
        {"player", to_json(result.player)},
    };
}

boost::json::object to_json(const LootDrop& drop) {
    return {
        {"drop_id", drop.drop_id},
        {"owner_player_id", drop.owner_player_id},
        {"scene_id", drop.scene_id},
        {"rewards", to_json_array(drop.rewards)},
        {"picked", drop.picked},
        {"picked_by_player_id", drop.picked_by_player_id},
        {"spawned_at", optional_unix_time(drop.spawned_at)},
        {"expires_at", optional_unix_time(drop.expires_at)},
        {"picked_at", optional_unix_time(drop.picked_at)},
    };
}

boost::json::object to_json(const LootPickupResult& result) {
    return {
        {"drop", to_json(result.drop)},
        {"grant", result.grant ? boost::json::value{to_json(*result.grant)} : boost::json::value{}},
        {"player", to_json(result.player)},
    };
}

boost::json::object to_json(const QuestObjectiveDefinition& objective) {
    return {
        {"objective_id", objective.objective_id},
        {"event_type", objective.event_type},
        {"target_id", objective.target_id},
        {"target_value", objective.target_value},
    };
}

boost::json::object to_json(const QuestDefinition& quest) {
    return {
        {"quest_id", quest.quest_id},
        {"version", quest.version},
        {"title", quest.title},
        {"description", quest.description},
        {"claim_mode", to_string(quest.claim_mode)},
        {"turn_in_npc_id", quest.turn_in_npc_id},
        {"completion_flag", quest.completion_flag},
        {"pre_quest_ids", to_json_array(quest.pre_quest_ids)},
        {"required_flags", to_json_array(quest.required_flags)},
        {"objectives", to_json_array(quest.objectives)},
        {"rewards", to_json_array(quest.rewards)},
        {"enabled", quest.enabled},
        {"created_at", optional_unix_time(quest.created_at)},
        {"updated_at", optional_unix_time(quest.updated_at)},
    };
}

boost::json::object to_json(const PlayerQuestObjective& objective) {
    return {
        {"objective_id", objective.objective_id},
        {"current_value", objective.current_value},
        {"target_value", objective.target_value},
    };
}

boost::json::object to_json(const PlayerQuest& quest) {
    return {
        {"player_id", quest.player_id},
        {"quest_id", quest.quest_id},
        {"quest_version", quest.quest_version},
        {"state", to_string(quest.state)},
        {"progress_version", quest.progress_version},
        {"objectives", to_json_array(quest.objectives)},
        {"accepted_at", optional_unix_time(quest.accepted_at)},
        {"completed_at", optional_unix_time(quest.completed_at)},
        {"claimed_at", optional_unix_time(quest.claimed_at)},
    };
}

boost::json::object to_json(const QuestSnapshot& snapshot) {
    return {
        {"quests", to_json_array(snapshot.quests)},
        {"story_flags", to_json_array(snapshot.story_flags)},
    };
}

boost::json::object to_json(const QuestEventResult& result) {
    boost::json::object object{
        {"snapshot", to_json(result.snapshot)},
        {"automatic_grants", to_json_array(result.automatic_grants)},
        {"duplicate_event", result.duplicate_event},
    };
    object["player"] = result.player ? boost::json::value{to_json(*result.player)} : boost::json::value{nullptr};
    return object;
}

boost::json::object to_json(const QuestClaimResult& result) {
    return {
        {"quest", to_json(result.quest)},
        {"grant", to_json(result.grant)},
        {"player", to_json(result.player)},
        {"already_claimed", result.already_claimed},
    };
}

boost::json::object to_json(const AnnouncementEntry& announcement) {
    return {
        {"announcement_id", announcement.announcement_id},
        {"channel", to_json(announcement.channel)},
        {"operator_id", announcement.operator_id},
        {"title", announcement.title},
        {"body", announcement.body},
        {"published_at", optional_unix_time(announcement.published_at)},
        {"expires_at", optional_unix_time(announcement.expires_at)},
    };
}

boost::json::object to_json(const RewardGrantRecord& grant) {
    return {
        {"grant_id", grant.grant_id},
        {"player_id", grant.player_id},
        {"source_type", grant.source_type},
        {"source_id", grant.source_id},
        {"rewards", to_json_array(grant.rewards)},
        {"granted_at", optional_unix_time(grant.granted_at)},
    };
}

boost::json::object to_json(const AuditLogEntry& audit) {
    return {
        {"audit_id", audit.audit_id},
        {"operator_id", audit.operator_id},
        {"action", audit.action},
        {"target_type", audit.target_type},
        {"target_id", audit.target_id},
        {"request_ip", audit.request_ip},
        {"user_agent", audit.user_agent},
        {"trace_id", audit.trace_id},
        {"success", audit.success},
        {"failure_reason", audit.failure_reason},
        {"duration_ms", audit.duration_ms},
        {"before_json", audit.before_json},
        {"after_json", audit.after_json},
        {"detail_json", audit.detail_json},
        {"created_at", optional_unix_time(audit.created_at)},
    };
}

boost::json::object to_json(const AuditLogWriteRequest& audit_request) {
    return {
        {"operator_id", audit_request.operator_id},
        {"action", audit_request.action},
        {"target_type", audit_request.target_type},
        {"target_id", audit_request.target_id},
        {"request_ip", audit_request.request_ip},
        {"user_agent", audit_request.user_agent},
        {"trace_id", audit_request.trace_id},
        {"success", audit_request.success},
        {"failure_reason", audit_request.failure_reason},
        {"duration_ms", audit_request.duration_ms},
        {"before_json", audit_request.before_json},
        {"after_json", audit_request.after_json},
        {"detail_json", audit_request.detail_json},
    };
}

boost::json::object to_json(const AdminPrincipal& admin) {
    return {
        {"account_id", admin.account_id},
        {"operator_name", admin.operator_name},
        {"active", admin.active},
        {"permissions", to_json_array(admin.permissions)},
        {"created_at", optional_unix_time(admin.created_at)},
        {"updated_at", optional_unix_time(admin.updated_at)},
    };
}

boost::json::object to_json(const AdminTask& task) {
    return {
        {"task_id", task.task_id},
        {"kind", to_string(task.kind)},
        {"status", task.status},
        {"operator_id", task.operator_id},
        {"target_type", task.target_type},
        {"target_id", task.target_id},
        {"request_ip", task.request_ip},
        {"user_agent", task.user_agent},
        {"trace_id", task.trace_id},
        {"request_json", task.request_json},
        {"result_json", task.result_json},
        {"failure_reason", task.failure_reason},
        {"execute_at", optional_unix_time(task.execute_at)},
        {"executed_at", optional_unix_time(task.executed_at)},
        {"created_at", optional_unix_time(task.created_at)},
        {"updated_at", optional_unix_time(task.updated_at)},
    };
}

boost::json::object to_json(const MailMessage& mail) {
    return {
        {"mail_id", mail.mail_id},
        {"player_id", mail.player_id},
        {"sender_name", mail.sender_name},
        {"title", mail.title},
        {"body", mail.body},
        {"rewards", to_json_array(mail.rewards)},
        {"read", mail.read},
        {"claimed", mail.claimed},
        {"deleted", mail.deleted},
        {"claim_grant_id", mail.claim_grant_id},
        {"sent_at", optional_unix_time(mail.sent_at)},
        {"expires_at", optional_unix_time(mail.expires_at)},
        {"read_at", optional_unix_time(mail.read_at)},
        {"claimed_at", optional_unix_time(mail.claimed_at)},
        {"deleted_at", optional_unix_time(mail.deleted_at)},
    };
}

boost::json::object to_json(const FriendLink& link) {
    return {
        {"player_id", link.player_id},
        {"friend_player_id", link.friend_player_id},
    };
}

boost::json::object to_json(const FriendRequest& request) {
    return {
        {"requester_player_id", request.requester_player_id},
        {"target_player_id", request.target_player_id},
        {"status", to_string(request.status)},
        {"created_at", optional_unix_time(request.created_at)},
    };
}

boost::json::object to_json(const ChatBlockEntry& block) {
    return {
        {"player_id", block.player_id},
        {"blocked_player_id", block.blocked_player_id},
        {"created_at", optional_unix_time(block.created_at)},
    };
}

boost::json::object to_json(const ChatMuteEntry& mute) {
    return {
        {"channel_id", mute.channel_id},
        {"muted_player_id", mute.muted_player_id},
        {"muted_by_player_id", mute.muted_by_player_id},
        {"reason", mute.reason},
        {"muted_until", optional_unix_time(mute.muted_until)},
    };
}

boost::json::object to_json(const ChannelUnreadCount& unread) {
    return {
        {"channel_id", unread.channel_id},
        {"unread_count", unread.unread_count},
    };
}

boost::json::object to_json(const FriendPresence& presence) {
    return {
        {"player_id", presence.player_id},
        {"online", presence.online},
        {"connection_count", presence.connection_count},
    };
}

boost::json::object to_json(const Session& session) {
    return {
        {"token", session.token},
        {"session_id", session.session_id},
        {"refresh_token", session.refresh_token},
        {"account_id", session.account_id},
        {"player_id", session.player_id},
        {"nickname", session.nickname},
        {"client_ip", session.client_ip},
        {"user_agent", session.user_agent},
        {"issued_at", to_unix_seconds(session.issued_at)},
        {"expires_at", to_unix_seconds(session.expires_at)},
    };
}

boost::json::object to_json(const ChatChannel& channel) {
    return {
        {"channel_id", channel.channel_id},
        {"kind", mf::chat::to_string(channel.kind)},
        {"scope_id", channel.scope_id},
        {"display_name", channel.display_name},
    };
}

boost::json::object to_json(const ChatMessage& message) {
    return {
        {"message_id", message.message_id},
        {"channel", to_json(message.channel)},
        {"kind", mf::chat::to_string(message.kind)},
        {"sender_player_id", message.sender_player_id},
        {"sender_nickname", message.sender_nickname},
        {"text", message.text},
        {"recalled", message.recalled},
        {"recalled_by_player_id", message.recalled_by_player_id},
        {"recalled_at", optional_unix_time(message.recalled_at)},
        {"sent_at", to_unix_seconds(message.sent_at)},
    };
}

boost::json::array to_json_array(const std::vector<std::string>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(boost::json::value{value});
    }
    return result;
}

boost::json::array to_json_array(const std::vector<PlayerProfile>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<CurrencyBalance>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<InventoryItem>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ItemDefinition>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<RewardEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<RewardTemplate>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<GachaPool>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<GachaPoolItem>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<GachaHistoryEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ShopProduct>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ShopPurchaseRecord>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<LootDrop>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<QuestObjectiveDefinition>& values) {
    boost::json::array result;
    for (const auto& value : values) result.push_back(to_json(value));
    return result;
}

boost::json::array to_json_array(const std::vector<QuestDefinition>& values) {
    boost::json::array result;
    for (const auto& value : values) result.push_back(to_json(value));
    return result;
}

boost::json::array to_json_array(const std::vector<PlayerQuestObjective>& values) {
    boost::json::array result;
    for (const auto& value : values) result.push_back(to_json(value));
    return result;
}

boost::json::array to_json_array(const std::vector<PlayerQuest>& values) {
    boost::json::array result;
    for (const auto& value : values) result.push_back(to_json(value));
    return result;
}

boost::json::array to_json_array(const std::vector<AnnouncementEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<RewardGrantRecord>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<AuditLogEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<AdminPrincipal>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<AdminTask>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<MailMessage>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<FriendLink>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<FriendRequest>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ChatBlockEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<BlacklistEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ChatMuteEntry>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ChannelUnreadCount>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<FriendPresence>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

boost::json::array to_json_array(const std::vector<ChatMessage>& values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(to_json(value));
    }
    return result;
}

}  // namespace mf::game 命名空间
