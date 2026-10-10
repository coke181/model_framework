#pragma once

// 文件职责：
// 定义领域仓储接口，隔离业务代码和具体存储实现。
// 调用流程：
// 业务层只依赖 AccountRepository/PlayerRepository/... 接口 ->
// 当前示例注入内存实现 -> 以后可替换为 MySQL/PostgreSQL/Redis 实现而不改业务层。

#include "mf/game/models.hpp"

#include <boost/asio/awaitable.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mf::agent {
class AgentRepository;
}

namespace mf::voice {
class VoiceRepository;
}

namespace mf::multiplayer {
class MultiplayerRoomRepository;
}

namespace mf::game {

class AccountRepository {
public:
    virtual ~AccountRepository() = default;

    // upsert_quick_account 保留“开发态快速登录”能力：
    // 第一次登录就创建 quick 账号，之后重复调用只更新展示名。
    // 如果 login_name 已被正式密码账号占用，则返回 nullopt，避免开发接口绕过密码体系。
    virtual std::optional<Account> upsert_quick_account(std::string login_name, std::string display_name) = 0;

    // create_password_account 对应正式注册流程：
    // 先由密码哈希服务生成 password_hash/password_scheme，再交给仓储落库。
    // 若 login_name 已存在，则返回 nullopt。
    virtual std::optional<Account> create_password_account(
        std::string login_name,
        std::string display_name,
        std::string email,
        std::string phone_number,
        std::string platform_channel,
        std::string register_source,
        std::string password_hash,
        std::string password_scheme) = 0;

    // find_password_account_by_login 专门给正式密码登录流程使用。
    // 它只返回可进行密码校验的账号记录，不把 quick 账号误当成正式账号。
    virtual std::optional<PasswordAccountRecord> find_password_account_by_login(
        const std::string& login_name) const = 0;
    virtual std::optional<Account> find_by_login_name(const std::string& login_name) const = 0;
    virtual std::optional<Account> update_password_credentials(
        const std::string& account_id,
        std::string password_hash,
        std::string password_scheme) = 0;
    virtual std::optional<PasswordResetTicket> create_password_reset_ticket(
        const std::string& login_name,
        const std::string& email,
        std::string reset_token,
        std::chrono::system_clock::time_point expires_at) = 0;
    virtual std::optional<Account> reset_password_with_ticket(
        const std::string& reset_token,
        std::string password_hash,
        std::string password_scheme) = 0;
    virtual std::optional<Account> set_account_status(
        const std::string& account_id,
        AccountStatus status,
        std::string reason) = 0;
    virtual std::optional<Account> record_login_success(
        const std::string& account_id,
        const std::string& client_ip) = 0;

    // 登录成功时只更新审计字段，不强制再查询整行账号。
    // MySQL 实现用这个入口减少一次“UPDATE 后 SELECT”的阻塞往返；
    // 内存实现沿用原有逻辑，保证不同仓储的行为一致。
    virtual void record_login_success_without_fetch(
        const std::string& account_id,
        const std::string& client_ip) {
        static_cast<void>(record_login_success(account_id, client_ip));
    }
    virtual std::optional<Account> record_login_failure(
        const std::string& account_id,
        std::int64_t lock_threshold,
        std::chrono::seconds lock_duration) = 0;
    virtual std::optional<Account> find_by_account_id(const std::string& account_id) const = 0;
};

class PlayerRepository {
public:
    virtual ~PlayerRepository() = default;

    // PlayerRepository 对应关系库里的玩家主资料表；
    // Redis 可以做热点缓存，但玩家资料的最终事实来源仍应是 MySQL。
    virtual PlayerProfile upsert_for_account(const Account& account) = 0;
    // 注册场景已知账号刚创建，不需要走登录兼容路径里的多次查找。
    // MySQL 实现可在一次连接租约内完成玩家主行和默认资产初始化。
    virtual PlayerProfile create_for_registration(const Account& account) {
        return upsert_for_account(account);
    }
    // 登录只需拿到玩家身份和基础主行字段；具体实现可跳过货币、背包等明细读取。
    virtual PlayerProfile upsert_for_login(const Account& account) {
        return upsert_for_account(account);
    }
    virtual std::optional<PlayerProfile> find_by_player_id(const std::string& player_id) const = 0;
    virtual std::vector<PlayerSummary> find_summaries_by_player_ids(
        const std::vector<std::string>& player_ids) const = 0;
    virtual std::vector<PlayerProfile> list_all() const = 0;
    // 后台玩家主表需要真正走数据库分页和筛选，
    // 避免数据量上来后先全量 list_all() 再在进程内过滤。
    virtual std::size_t count_filtered(
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const = 0;
    virtual std::vector<PlayerProfile> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const = 0;
    virtual PlayerAdminPage list_admin_rows(
        std::size_t limit,
        const std::optional<PlayerPageCursor>& cursor,
        const std::optional<std::string>& player_id_filter,
        const std::optional<std::string>& account_id_filter,
        const std::optional<std::string>& nickname_filter) const = 0;

    // apply_rewards 是“真正改资产”的落点。
    // grant_id 由上层生成，用于在玩家资产写入层再做一次幂等保护。
    virtual std::optional<PlayerProfile> apply_rewards(
        const std::string& player_id,
        const std::vector<RewardEntry>& rewards,
        const std::string& grant_id) = 0;
    // spend_currency 是消耗货币的统一落点。
    // 抽卡、商城购买、活动兑换都应该从这里扣资源，避免各业务直接改玩家 JSON。
    virtual std::optional<PlayerProfile> spend_currency(
        const std::string& player_id,
        const std::string& currency_id,
        std::int64_t amount) = 0;
    // 直接设置某个堆叠道具数量，主要给受控客户端流程或后台调试使用。
    // quantity 为 0 时表示从背包中移除该 item_id。
    virtual std::optional<PlayerProfile> set_inventory_item_quantity(
        const std::string& player_id,
        const std::string& item_id,
        std::int64_t quantity) = 0;
    virtual InventoryPage list_inventory_items(
        const std::string& player_id,
        std::size_t limit,
        const std::optional<std::string>& cursor = std::nullopt) const = 0;
};

class ItemCatalogRepository {
public:
    virtual ~ItemCatalogRepository() = default;

    // 物品配置仓储负责维护“物品是什么”，不直接代表玩家拥有数量。
    // GM 后台可以用它新增/修改物品，UE 可以用它展示图标、描述和稀有度。
    virtual ItemDefinition upsert_item(ItemDefinition item) = 0;
    virtual std::optional<ItemDefinition> find_by_item_id(const std::string& item_id) const = 0;
    virtual std::vector<ItemDefinition> list_items(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const = 0;
    virtual std::size_t count_items(
        const std::optional<std::string>& item_id_filter,
        const std::optional<std::string>& name_filter,
        const std::optional<std::string>& rarity_filter,
        const std::optional<bool>& enabled_filter) const = 0;
};

class GachaRepository {
public:
    virtual ~GachaRepository() = default;

    // 抽卡仓储负责保存卡池配置、奖品配置、玩家保底状态和抽卡历史。
    // 服务层只关心这些接口，不直接依赖 MySQL 表结构。
    virtual GachaPool upsert_pool(GachaPool pool) = 0;
    virtual bool delete_pool(const std::string& pool_id) = 0;
    virtual std::optional<GachaPool> find_pool(const std::string& pool_id) const = 0;
    virtual std::vector<GachaPool> list_pools(bool include_disabled, std::chrono::system_clock::time_point now) const = 0;
    virtual GachaPoolItem upsert_pool_item(GachaPoolItem item) = 0;
    virtual bool delete_pool_item(const std::string& pool_id, const std::string& item_key) = 0;
    virtual std::vector<GachaPoolItem> list_pool_items(const std::string& pool_id) const = 0;
    virtual GachaPlayerState get_player_state(const std::string& player_id, const std::string& pool_id) const = 0;
    virtual GachaPlayerState save_player_state(GachaPlayerState state) = 0;
    virtual void append_history(const std::vector<GachaHistoryEntry>& entries) = 0;
    virtual GachaHistoryPage list_history(
        const std::string& player_id,
        const std::string& pool_id,
        std::size_t limit,
        const std::optional<GachaHistoryCursor>& cursor = std::nullopt) const = 0;
};

class GachaTransactionRepository {
public:
    virtual ~GachaTransactionRepository() = default;

    // draw_once 在同一事务内完成：锁保底状态 -> 扣货币 -> 随机抽取 ->
    // 更新保底 -> 写历史 -> 发奖励。任何一步失败都会整体回滚。
    // pick_item 由服务层提供随机逻辑，仓储层只负责保证并发时状态不丢。
    virtual GachaDrawTransactionResult draw_once(
        const std::string& player_id,
        const std::string& pool_id,
        const GachaPool& pool,
        const std::vector<GachaPoolItem>& items,
        std::int64_t count,
        std::int64_t total_cost,
        const std::vector<std::string>& history_ids,
        const std::string& grant_id,
        std::chrono::system_clock::time_point drawn_at,
        const std::function<GachaPoolItem(const GachaPlayerState&)>& pick_item) = 0;
};

class ShopRepository {
public:
    virtual ~ShopRepository() = default;

    // 商城仓储负责保存商品配置和购买流水。
    // 购买扣货币和发奖励不放在这里做，而是由 ShopService 协调 PlayerRepository/RewardService。
    virtual ShopProduct upsert_product(ShopProduct product) = 0;
    virtual bool delete_product(const std::string& product_id) = 0;
    virtual std::optional<ShopProduct> find_product(const std::string& product_id) const = 0;
    virtual std::vector<ShopProduct> list_products(
        bool include_disabled,
        std::chrono::system_clock::time_point now) const = 0;
    virtual std::int64_t count_purchases(const std::string& player_id, const std::string& product_id) const = 0;
    virtual ShopPurchaseRecord append_purchase(ShopPurchaseRecord purchase) = 0;
    virtual ShopPurchasePage list_purchases(
        const std::string& player_id,
        std::size_t limit,
        const std::optional<ShopPurchaseCursor>& cursor = std::nullopt) const = 0;
};

class LootRepository {
public:
    virtual ~LootRepository() = default;

    // 掉落仓储负责记录掉落生成、查询和拾取状态。
    // 真正发奖仍由 LootService 调 RewardService，避免绕过奖励流水。
    virtual LootDrop create_drop(LootDrop drop) = 0;
    virtual std::optional<LootDrop> find_drop(const std::string& drop_id) const = 0;
    virtual std::optional<LootDrop> mark_picked(
        const std::string& drop_id,
        const std::string& picked_by_player_id,
        std::chrono::system_clock::time_point picked_at) = 0;
    virtual LootDropPage list_drops(
        const std::string& owner_player_id,
        bool include_picked,
        std::chrono::system_clock::time_point now,
        std::size_t limit,
        const std::optional<LootDropCursor>& cursor = std::nullopt) const = 0;
};

class RewardTransactionRepository {
public:
    virtual ~RewardTransactionRepository() = default;

    // grant_once 在同一个存储事务里写奖励流水并更新玩家资产。
    virtual RewardGrantTransactionResult grant_once(
        const std::string& player_id,
        const std::string& grant_id,
        const std::string& source_type,
        const std::string& source_id,
        const std::vector<RewardEntry>& rewards) = 0;
};

class LootTransactionRepository {
public:
    virtual ~LootTransactionRepository() = default;

    // pickup_once 在同一事务内锁定掉落、发奖并标记已拾取。
    virtual LootPickupTransactionResult pickup_once(
        const std::string& player_id,
        const std::string& drop_id,
        std::chrono::system_clock::time_point now) = 0;
};

class ShopTransactionRepository {
public:
    virtual ~ShopTransactionRepository() = default;

    // purchase_once 在同一事务内完成：占限购名额 -> 扣货币 -> 发奖励 -> 写购买流水。
    // 任何一步失败都会回滚，不会出现“扣了钱没发货”或“超过限购”的中间状态。
    virtual ShopPurchaseTransactionResult purchase_once(
        const std::string& purchase_id,
        const std::string& player_id,
        const ShopProduct& product,
        std::chrono::system_clock::time_point purchased_at) = 0;
};

class QuestRepository {
public:
    virtual ~QuestRepository() = default;

    virtual QuestDefinition upsert_definition(QuestDefinition definition) = 0;
    virtual std::vector<QuestDefinition> list_definitions(bool include_disabled) const = 0;
    virtual std::optional<QuestDefinition> find_definition(const std::string& quest_id) const = 0;
    virtual QuestSnapshot get_snapshot(const std::string& player_id) const = 0;
    virtual std::optional<PlayerQuest> accept(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id,
        std::string& failure_code) = 0;
    virtual QuestEventResult apply_event(
        const std::string& player_id,
        const std::string& event_id,
        const std::string& event_type,
        const std::string& target_id,
        std::int64_t amount,
        const std::string& source_id,
        std::string& failure_code) = 0;
    virtual std::optional<QuestClaimResult> claim(
        const std::string& player_id,
        const std::string& quest_id,
        const std::string& request_id,
        const std::string& npc_id,
        std::string& failure_code) = 0;
};

class FriendRepository {
public:
    virtual ~FriendRepository() = default;

    virtual bool add_friendship(const std::string& player_id, const std::string& friend_player_id) = 0;
    virtual std::vector<FriendLink> list_friends(const std::string& player_id) const = 0;
    virtual std::vector<FriendLink> list_friends_by_last_message(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<FriendPageCursor>& cursor = std::nullopt) const = 0;
    virtual bool send_friend_request(const std::string& requester_player_id, const std::string& target_player_id) = 0;
    virtual std::vector<FriendRequest> list_incoming_requests(const std::string& player_id) const = 0;
    virtual std::vector<FriendRequest> list_outgoing_requests(const std::string& player_id) const = 0;
    virtual bool respond_friend_request(
        const std::string& requester_player_id,
        const std::string& target_player_id,
        bool accept) = 0;
    virtual bool set_chat_block(
        const std::string& player_id,
        const std::string& blocked_player_id,
        bool blocked) = 0;
    virtual bool is_chat_blocked(const std::string& player_id, const std::string& blocked_player_id) const = 0;
    virtual std::vector<ChatBlockEntry> list_chat_blocks(const std::string& player_id) const = 0;
};

class SessionRepository {
public:
    virtual ~SessionRepository() = default;

    // Session 持久化单独拆出来，
    // 这样将来切换 token 策略时，不需要重写 handler 或 websocket 鉴权逻辑。
    // 这个仓储在生产环境里更适合由 Redis 承担，因为会话天然是高频读写且可过期的数据。
    virtual void save(Session session) = 0;
    // 异步路由优先调用这个入口。默认实现保持旧仓储兼容；
    // Redis 实现会覆写为真正的 async_exec，不占用业务线程等待网络。
    virtual boost::asio::awaitable<void> save_async(Session session) {
        save(std::move(session));
        co_return;
    }
    // 原子地撤销该玩家所有旧会话并写入新会话，用于 single_session 登录语义。
    virtual void save_single_session(Session session) = 0;
    virtual std::optional<Session> find_by_token(const std::string& token) const = 0;
    virtual std::optional<Session> find_by_refresh_token(const std::string& refresh_token) const = 0;
    // 原子读取并撤销 refresh token 对应的旧会话。
    // TokenService 刷新会话时使用这个入口，避免“先查再删”的并发重放窗口。
    virtual std::optional<Session> consume_refresh_token(const std::string& refresh_token) = 0;
    virtual boost::asio::awaitable<std::optional<Session>> consume_refresh_token_async(
        std::string refresh_token) {
        co_return consume_refresh_token(refresh_token);
    }
    virtual std::vector<Session> list_by_player_id(const std::string& player_id) const = 0;
    virtual bool revoke_by_token(const std::string& token) = 0;
    virtual bool revoke_by_session_id(const std::string& session_id) = 0;
    virtual std::size_t revoke_all_for_player(
        const std::string& player_id,
        const std::optional<std::string>& except_session_id = std::nullopt) = 0;
};

// 聊天历史分页结果。messages 保持“旧 -> 新”顺序，next_cursor 指向更早的一页。
struct ChatHistoryPage {
    std::vector<ChatMessage> messages;
    std::string next_cursor;
    bool has_more = false;
};

struct ChatOutboxBatchResult {
    std::vector<ChatMessage> messages;
    std::size_t pending_count = 0;
    std::size_t failed_count = 0;
    std::int64_t oldest_pending_age_seconds = 0;
};

struct ChatOutboxHealth {
    std::size_t pending_count = 0;
    std::size_t failed_count = 0;
    std::int64_t oldest_pending_age_seconds = 0;
};

class ChatRepository {
public:
    virtual ~ChatRepository() = default;

    // ChatRepository 负责消息持久化，适合把最近窗口缓存放 Redis，
    // 同时把完整历史或关键消息落到 MySQL。
    // append() 返回最终持久化后的消息对象，
    // 这样实时广播可以直接复用数据库/缓存层补齐后的 message_id 等字段。
    virtual ChatMessage append(ChatMessage message) = 0;
    // Outbox 是可恢复的持久化队列。MySQL 实现会先把完整消息写入
    // chat_message_outbox，进程重启后 worker 可以继续处理未完成记录。
    // 内存仓储默认关闭该能力，保持本地 review 模式的简单语义。
    virtual bool supports_outbox() const { return false; }
    virtual ChatMessage append_to_outbox(ChatMessage message) {
        return append(std::move(message));
    }
    // 把一批待处理消息原子地写入历史表并从 outbox 删除。
    virtual ChatOutboxBatchResult drain_outbox_batch(std::size_t) { return {}; }
    virtual ChatOutboxHealth outbox_health() const { return {}; }
    virtual std::size_t retry_failed_outbox(std::size_t) { return 0; }
    // 批量写入接口。默认实现逐条调用 append，保证内存仓储和旧仓储兼容；
    // MySQL 仓储可覆盖为“一个连接租约 + 一个事务”的批量写入。
    virtual std::vector<ChatMessage> append_batch(std::vector<ChatMessage> messages) {
        std::vector<ChatMessage> persisted;
        persisted.reserve(messages.size());
        for (auto& message : messages) {
            persisted.push_back(append(std::move(message)));
        }
        return persisted;
    }
    virtual std::optional<ChatMessage> recall(
        const std::string& channel_id,
        const std::string& message_id,
        const std::string& recalled_by_player_id) = 0;
    virtual std::vector<ChatMessage> recent_messages(const std::string& channel_id, std::size_t limit) const = 0;
    // 按时间游标分页读取历史，避免深 offset 全表扫描。
    // cursor 为空表示从最新消息开始；limit 会被实现夹在 [1, 100]。
    // cursor 采用 URL 安全的 "<sent_at_seconds>-<message_id>" 编码。
    virtual ChatHistoryPage history_page(
        const std::string& channel_id,
        std::string_view cursor,
        std::size_t limit) const = 0;
    virtual std::vector<ChatMessage> latest_messages(
        const std::vector<std::string>& channel_ids,
        std::size_t limit) const = 0;
};

class ChatModerationRepository {
public:
    virtual ~ChatModerationRepository() = default;

    virtual ChatMuteEntry mute_player(
        const std::string& channel_id,
        const std::string& muted_player_id,
        const std::string& muted_by_player_id,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> muted_until) = 0;
    virtual void clear_mute(const std::string& channel_id, const std::string& muted_player_id) = 0;
    virtual std::optional<ChatMuteEntry> find_active_mute(
        const std::string& channel_id,
        const std::string& muted_player_id) const = 0;
};

class AuthSecurityRepository {
public:
    virtual ~AuthSecurityRepository() = default;

    virtual CaptchaChallenge create_captcha(
        std::string prompt,
        std::string expected_answer,
        std::chrono::system_clock::time_point expires_at) = 0;
    virtual bool verify_and_consume_captcha(const std::string& challenge_id, const std::string& answer) = 0;
    virtual LoginRiskState inspect_login_risk(
        const std::string& login_name,
        const std::string& client_ip) const = 0;
    // 登录固定要同时读取黑名单和风控状态；Redis 实现可在一次 pipeline 中完成。
    virtual LoginRiskState inspect_login_access(
        const std::string& login_name,
        const std::string& client_ip) const {
        auto state = inspect_login_risk(login_name, client_ip);
        state.blacklisted = is_blacklisted("ip", client_ip) || is_blacklisted("login_name", login_name);
        return state;
    }
    virtual LoginRiskState record_login_failure(
        const std::string& login_name,
        const std::string& client_ip,
        std::chrono::seconds window,
        std::int64_t captcha_threshold,
        std::int64_t rate_limit_threshold,
        std::chrono::seconds block_duration) = 0;
    virtual void clear_login_failures(const std::string& login_name, const std::string& client_ip) = 0;
    // 注册限流与登录失败风控分开存储，避免注册流量污染登录失败计数。
    // 通常同时限制 IP+登录名组合和单 IP 总量，防止批量注册消耗密码哈希 CPU。
    virtual LoginRiskState record_registration_attempt(
        const std::string& login_name,
        const std::string& client_ip,
        std::chrono::seconds window,
        std::int64_t max_per_login_ip,
        std::int64_t max_per_ip) = 0;
    virtual void clear_registration_attempts(
        const std::string& login_name,
        const std::string& client_ip) = 0;
    virtual bool is_blacklisted(const std::string& subject_type, const std::string& subject_value) const = 0;
    // 登录入口固定需要检查 IP 和登录名两个黑名单；Redis 实现可用一次 pipeline
    // 完成两个 GET，减少网络往返，其他仓储沿用两个独立查询的兼容行为。
    virtual bool is_login_blacklisted(
        const std::string& login_name,
        const std::string& client_ip) const {
        return is_blacklisted("ip", client_ip) || is_blacklisted("login_name", login_name);
    }
    virtual BlacklistEntry add_blacklist_entry(
        std::string subject_type,
        std::string subject_value,
        std::string reason,
        std::optional<std::chrono::system_clock::time_point> expires_at) = 0;
    virtual std::vector<BlacklistEntry> list_blacklist_entries(std::size_t limit) const = 0;
};

class RewardRepository {
public:
    virtual ~RewardRepository() = default;

    virtual std::optional<RewardGrantRecord> find_by_grant_id(const std::string& grant_id) const = 0;
    virtual RewardGrantRecord record_grant(
        std::string grant_id,
        const std::string& player_id,
        std::string source_type,
        std::string source_id,
        std::vector<RewardEntry> rewards) = 0;
    virtual std::vector<RewardGrantRecord> list_by_player_id(
        const std::string& player_id,
        std::size_t limit) const = 0;
    // 后台查询奖励流水时，需要按来源类型 / 来源 ID 做筛选并拿精确总数。
    virtual std::size_t count_filtered(
        const std::string& player_id,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const = 0;
    virtual std::vector<RewardGrantRecord> list_filtered(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const = 0;
};

class RewardTemplateRepository {
public:
    virtual ~RewardTemplateRepository() = default;

    virtual RewardTemplate upsert_template(
        std::string template_id,
        std::string name,
        std::vector<RewardEntry> rewards) = 0;
    virtual std::optional<RewardTemplate> find_by_template_id(const std::string& template_id) const = 0;
    virtual std::vector<RewardTemplate> list_all() const = 0;
};

class AnnouncementRepository {
public:
    virtual ~AnnouncementRepository() = default;

    virtual AnnouncementEntry publish(
        ChatChannel channel,
        std::string operator_id,
        std::string title,
        std::string body,
        std::chrono::system_clock::time_point published_at,
        std::optional<std::chrono::system_clock::time_point> expires_at) = 0;
    virtual std::vector<AnnouncementEntry> list_recent(
        std::size_t limit,
        const std::optional<ChatChannelKind>& kind_filter,
        const std::optional<std::string>& scope_filter,
        std::chrono::system_clock::time_point now) const = 0;
};

class MailRepository {
public:
    virtual ~MailRepository() = default;

    virtual MailMessage send_mail(MailMessage mail) = 0;
    virtual std::vector<MailMessage> list_inbox(const std::string& player_id, bool include_deleted) const = 0;
    virtual std::vector<MailMessage> list_inbox_cursor(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        const std::optional<MailPageCursor>& cursor) const = 0;
    // 后台查邮件时需要支持已读 / 已领取筛选，以及分页总数。
    virtual std::size_t count_filtered(
        const std::string& player_id,
        bool include_deleted,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const = 0;
    virtual std::vector<MailMessage> list_filtered(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        std::size_t offset,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const = 0;
    virtual std::optional<MailMessage> find_mail(
        const std::string& player_id,
        const std::string& mail_id) const = 0;
    virtual std::optional<MailMessage> mark_read(
        const std::string& player_id,
        const std::string& mail_id,
        std::chrono::system_clock::time_point read_at) = 0;
    virtual std::optional<MailMessage> mark_claimed(
        const std::string& player_id,
        const std::string& mail_id,
        const std::string& claim_grant_id,
        std::chrono::system_clock::time_point claimed_at) = 0;
    virtual std::optional<MailMessage> mark_deleted(
        const std::string& player_id,
        const std::string& mail_id,
        std::chrono::system_clock::time_point deleted_at) = 0;
};

class AuditLogRepository {
public:
    virtual ~AuditLogRepository() = default;

    virtual AuditLogEntry append(AuditLogWriteRequest request) = 0;
    virtual std::vector<AuditLogEntry> list_recent(std::size_t limit) const = 0;
    // count_filtered 返回满足筛选条件的总条数，
    // 给后台分页接口返回精确 total，避免只靠当前页条数推测总量。
    virtual std::size_t count_filtered(
        const std::optional<std::string>& action_filter,
        const std::optional<std::string>& operator_filter,
        const std::optional<bool>& success_filter) const = 0;
    virtual AuditLogPage list_recent_filtered(
        std::size_t limit,
        const std::optional<AuditLogCursor>& cursor,
        const std::optional<std::string>& action_filter,
        const std::optional<std::string>& operator_filter,
        const std::optional<bool>& success_filter) const = 0;
};

class AdminRepository {
public:
    virtual ~AdminRepository() = default;

    virtual AdminPrincipal upsert_admin(
        const std::string& account_id,
        std::string operator_name,
        bool active,
        std::vector<std::string> permissions) = 0;
    virtual std::optional<AdminPrincipal> find_by_account_id(const std::string& account_id) const = 0;
    virtual std::vector<AdminPrincipal> list_all() const = 0;
    // count_filtered 返回筛选后的管理员总数，
    // 这样 /admin/users 的分页 total 可以直接来自仓储层真实计数。
    virtual std::size_t count_filtered(
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const = 0;
    virtual std::vector<AdminPrincipal> list_filtered(
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& operator_name_filter,
        const std::optional<bool>& active_filter) const = 0;
    virtual bool has_permission(const std::string& account_id, const std::string& permission) const = 0;
};

class AdminTaskRepository {
public:
    virtual ~AdminTaskRepository() = default;

    virtual AdminTask save(AdminTask task) = 0;
    virtual AdminTaskPage list_recent(
        std::size_t limit,
        const std::optional<AdminTaskCursor>& cursor,
        const std::optional<AdminTaskKind>& kind,
        const std::optional<std::string>& status) const = 0;
    // count_filtered 给后台任务分页返回精确 total。
    virtual std::size_t count_filtered(
        const std::optional<AdminTaskKind>& kind,
        const std::optional<std::string>& status) const = 0;
    virtual std::optional<AdminTask> find_by_id(const std::string& task_id) const = 0;
    virtual std::vector<AdminTask> claim_due_tasks(
        AdminTaskKind kind,
        std::chrono::system_clock::time_point now,
        std::size_t limit) = 0;
    virtual std::optional<AdminTask> mark_completed(
        const std::string& task_id,
        std::string result_json,
        std::chrono::system_clock::time_point executed_at) = 0;
    virtual std::optional<AdminTask> mark_failed(
        const std::string& task_id,
        std::string failure_reason,
        std::string result_json,
        std::chrono::system_clock::time_point executed_at) = 0;
};

struct RepositoryBundle {
    // RepositoryBundle 是示例应用里一个简单的组合根。
    // 如果以后需要，更成熟的服务容器可以替换它。
    std::shared_ptr<AccountRepository> accounts;
    std::shared_ptr<mf::agent::AgentRepository> agent;
    std::shared_ptr<PlayerRepository> players;
    std::shared_ptr<ItemCatalogRepository> item_catalog;
    std::shared_ptr<GachaRepository> gacha;
    std::shared_ptr<GachaTransactionRepository> gacha_transactions;
    std::shared_ptr<ShopRepository> shop;
    std::shared_ptr<LootRepository> loot;
    std::shared_ptr<RewardTransactionRepository> reward_transactions;
    std::shared_ptr<LootTransactionRepository> loot_transactions;
    std::shared_ptr<ShopTransactionRepository> shop_transactions;
    std::shared_ptr<QuestRepository> quests;
    std::shared_ptr<mf::multiplayer::MultiplayerRoomRepository> multiplayer_rooms;
    std::shared_ptr<mf::voice::VoiceRepository> voice;
    std::shared_ptr<FriendRepository> friends;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<ChatRepository> chat;
    std::shared_ptr<ChatModerationRepository> chat_moderation;
    std::shared_ptr<AuthSecurityRepository> auth_security;
    std::shared_ptr<RewardRepository> rewards;
    std::shared_ptr<RewardTemplateRepository> reward_templates;
    std::shared_ptr<AnnouncementRepository> announcements;
    std::shared_ptr<MailRepository> mails;
    std::shared_ptr<AuditLogRepository> audit_logs;
    std::shared_ptr<AdminRepository> admins;
    std::shared_ptr<AdminTaskRepository> admin_tasks;
};

RepositoryBundle make_in_memory_repositories();

}  // namespace mf::game 命名空间
