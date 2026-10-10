#pragma once

// 文件职责：
// 定义游戏领域模型，包括账号、玩家、好友、会话、聊天、邮件、奖励。
// 调用流程：
// 登录、玩家资料、好友、聊天等模块共享这些结构 ->
// 仓储层负责存取它们，API 层和聊天网关负责把它们转换成对外协议。

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace mf::game {

enum class AccountAuthMode {
    quick,
    password,
};

enum class AccountStatus {
    active,
    frozen,
    banned,
};

enum class ChatChannelKind {
    lobby,
    room,
    friend_direct,
};

enum class ChatMessageKind {
    text,
    system,
};

enum class FriendRequestStatus {
    pending_incoming,
    pending_outgoing,
    accepted,
};

enum class RewardKind {
    gold,
    currency,
    item,
    attachment_template,
};

enum class QuestState {
    accepted,
    completed,
    claimed,
};

enum class QuestClaimMode {
    automatic,
    manual,
    npc,
};

struct Account {
    // Account 表示外层登录身份。
    // 在很多游戏里，它会因为平台接入或登录体系而和角色/玩家数据分开存在。
    std::string account_id;
    std::string login_name;
    std::string display_name;
    std::string email;
    std::string phone_number;
    std::string platform_channel;
    std::string register_source;
    AccountAuthMode auth_mode = AccountAuthMode::quick;
    AccountStatus status = AccountStatus::active;
    std::string status_reason;
    std::int64_t failed_login_attempts = 0;
    std::optional<std::chrono::system_clock::time_point> locked_until;
    std::optional<std::chrono::system_clock::time_point> last_login_at;
    std::string last_login_ip;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct PasswordAccountRecord {
    // PasswordAccountRecord 是“可用于密码登录”的账号视图，
    // 它在普通 Account 基础上额外带出密码哈希，供登录校验使用。
    Account account;
    std::string password_hash;
    std::string password_scheme;
    std::string reset_token;
    std::optional<std::chrono::system_clock::time_point> reset_token_expires_at;
    // 登录查询可以顺便带出玩家身份，避免认证成功后再次查询 player_profile。
    // 没有角色的历史账号保持为空，由 AccountService 回退到原来的流程。
    std::optional<std::string> player_id;
};

struct SessionIssueOptions {
    // single_session=true 表示签发新会话前，先把同玩家旧会话踢掉。
    // 这对账号密码登录更安全；开发态 quick-login 则通常允许多开。
    bool single_session = false;
    std::string client_ip;
    std::string user_agent;
};

struct CurrencyBalance {
    // CurrencyBalance 让玩家资料可以持有多种货币，而不是只写死 gold。
    std::string currency_id;
    std::int64_t amount = 0;
};

struct InventoryItem {
    // InventoryItem 表示背包里的一个“堆叠格”。
    // item_id 标识物品类型，quantity 表示当前堆叠数量，
    // instance_id 让以后扩展到不可堆叠实例、耐久、附魔属性时仍有稳定锚点。
    std::string instance_id;
    std::string item_id;
    std::int64_t quantity = 0;
};

struct InventoryPage {
    // InventoryPage 是大背包场景下的 keyset 分页结果；items 只包含当前页。
    // next_cursor 为上一页最后一个 item_id，has_more 标记是否还有更早数据，
    // 这样避免每次打开背包都执行 COUNT(*) 和 OFFSET 深分页。
    std::vector<InventoryItem> items;
    std::string next_cursor;
    bool has_more = false;
};

struct ItemDefinition {
    // ItemDefinition 是“物品配置表”的一行。
    // 它描述物品是什么：名字、描述、图标、稀有度、最大堆叠和分类；
    // 玩家真正拥有多少个仍然放在背包/奖励逻辑里，避免配置和资产混在一起。
    std::string item_id;
    std::string name;
    std::string description;
    std::string icon;
    std::string rarity;
    std::int64_t max_stack = 1;
    std::string item_type;
    bool enabled = true;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct PlayerProfile {
    // PlayerProfile 是登录后返回给客户端的“游戏内身份”。
    // 当前起步模型假设一个账号只对应一个玩家资料。
    std::string player_id;
    std::string account_id;
    std::string nickname;
    std::int64_t gold = 0;
    std::int64_t gem = 0;
    std::vector<CurrencyBalance> currencies;
    std::vector<InventoryItem> inventory;
};

struct PlayerSummary {
    // PlayerSummary 只承载好友列表、聊天列表等轻量场景所需的身份字段。
    // 与 PlayerProfile 的差别是它不加载货币和背包，避免列表接口逐好友拼装完整资料。
    std::string player_id;
    std::string account_id;
    std::string nickname;
};

struct PlayerPageCursor {
    // PlayerPageCursor 是后台玩家主表 keyset 游标，
    // 用 (created_at, player_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_created_at = 0;
    std::string last_player_id;
};

struct PlayerAdminRow {
    // PlayerAdminRow 是给后台玩家主表使用的聚合行模型。
    // 它把玩家主资料、账号状态、最后登录时间和大厅禁言状态预先拼到一行里，
    // 让后台列表页可以一次查询拿全，而不是逐行补查。
    std::string player_id;
    std::string account_id;
    std::string nickname;
    std::int64_t gold = 0;
    std::int64_t gem = 0;
    AccountStatus status = AccountStatus::active;
    std::string mute_channel_id;
    std::optional<std::chrono::system_clock::time_point> muted_until;
    std::optional<std::chrono::system_clock::time_point> last_login_at;
    std::string last_login_ip;
};

struct PlayerAdminPage {
    std::vector<PlayerAdminRow> rows;
    std::int64_t next_created_at = 0;
    std::string next_player_id;
    bool has_more = false;
};

struct RewardEntry {
    // RewardEntry 把“发奖励”先抽象成统一资源项，
    // 这样邮件附件、GM 发奖、活动奖励都能走同一套结构。
    RewardKind kind = RewardKind::gold;
    std::string resource_id;
    std::int64_t amount = 0;
};

struct RewardTemplate {
    // RewardTemplate 是“配置化附件”或“模板化奖励”的事实模型。
    // 邮件、活动、GM 发奖都可以只引用 template_id，再由服务层展开成具体奖励项。
    std::string template_id;
    std::string name;
    std::vector<RewardEntry> rewards;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct RewardGrantRecord {
    // RewardGrantRecord 是奖励流水与幂等锚点。
    // grant_id 稳定后，同一笔奖励重复执行时可以安全识别并避免重复加资产。
    std::string grant_id;
    std::string player_id;
    std::string source_type;
    std::string source_id;
    std::vector<RewardEntry> rewards;
    std::optional<std::chrono::system_clock::time_point> granted_at;
};

struct RewardGrantTransactionResult {
    // 原子发奖仓储返回奖励流水和发奖后的玩家快照。
    // already_granted=true 表示本次 grant_id 以前已经成功执行，资产不会再次增加。
    std::optional<RewardGrantRecord> grant;
    std::optional<PlayerProfile> player;
    bool already_granted = false;
};

struct QuestObjectiveDefinition {
    // Objective 只描述服务端允许接收的事件和目标值，客户端不能直接提交完成状态。
    std::string objective_id;
    std::string event_type;
    std::string target_id;
    std::int64_t target_value = 1;
};

struct QuestDefinition {
    // QuestDefinition 是任务配置。奖励、前置条件和目标都以服务端配置为准。
    std::string quest_id;
    std::int64_t version = 1;
    std::string title;
    std::string description;
    QuestClaimMode claim_mode = QuestClaimMode::manual;
    std::string turn_in_npc_id;
    std::string completion_flag;
    std::vector<std::string> pre_quest_ids;
    std::vector<std::string> required_flags;
    std::vector<QuestObjectiveDefinition> objectives;
    std::vector<RewardEntry> rewards;
    bool enabled = true;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct PlayerQuestObjective {
    std::string objective_id;
    std::int64_t current_value = 0;
    std::int64_t target_value = 0;
};

struct PlayerQuest {
    // PlayerQuest 是玩家永久任务状态；progress_version 用于 UE 判断快照是否过期。
    std::string player_id;
    std::string quest_id;
    std::int64_t quest_version = 1;
    QuestState state = QuestState::accepted;
    std::int64_t progress_version = 0;
    std::vector<PlayerQuestObjective> objectives;
    std::optional<std::chrono::system_clock::time_point> accepted_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::optional<std::chrono::system_clock::time_point> claimed_at;
};

struct QuestSnapshot {
    std::vector<PlayerQuest> quests;
    std::vector<std::string> story_flags;
};

struct QuestEventResult {
    QuestSnapshot snapshot;
    std::vector<RewardGrantRecord> automatic_grants;
    std::optional<PlayerProfile> player;
    bool duplicate_event = false;
};

struct QuestClaimResult {
    PlayerQuest quest;
    RewardGrantRecord grant;
    PlayerProfile player;
    bool already_claimed = false;
};

struct GachaPool {
    // GachaPool 是一个抽卡池的主配置。
    // pool_id 由后台配置，例如 limited_role_202606；cost_* 表示每抽消耗哪种货币和多少。
    std::string pool_id;
    std::string pool_type = "standard";
    std::string name;
    std::string description;
    std::string banner_icon;
    std::string cost_currency_id = "gem";
    std::int64_t cost_amount_per_draw = 160;
    std::int64_t pity_5 = 90;
    std::int64_t pity_4 = 10;
    bool enabled = true;
    std::optional<std::chrono::system_clock::time_point> starts_at;
    std::optional<std::chrono::system_clock::time_point> ends_at;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct GachaPoolItem {
    // GachaPoolItem 表示卡池里的一个可抽中奖品。
    // weight 是基础权重；rarity 用 3/4/5 这类数值，方便做保底判断和前端展示。
    std::string pool_id;
    std::string item_key;
    std::int64_t rarity = 3;
    std::int64_t weight = 1;
    bool is_up = false;
    RewardEntry reward;
};

struct GachaPlayerState {
    // 每个玩家在每个卡池都有独立保底状态。
    // pity_5_count 表示距离上次 5 星之后已经抽了多少次，pity_4_count 同理。
    std::string player_id;
    std::string pool_id;
    std::int64_t pity_5_count = 0;
    std::int64_t pity_4_count = 0;
    std::int64_t total_draws = 0;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct GachaHistoryEntry {
    // GachaHistoryEntry 是每一次抽卡的不可抵赖记录。
    // 后续做客服查询、争议排查、概率审计都依赖它。
    std::string history_id;
    std::string player_id;
    std::string pool_id;
    std::string item_key;
    std::int64_t rarity = 3;
    bool is_up = false;
    RewardEntry reward;
    std::int64_t pity_5_before = 0;
    std::int64_t pity_4_before = 0;
    std::optional<std::chrono::system_clock::time_point> drawn_at;
};

struct GachaHistoryCursor {
    // GachaHistoryCursor 是抽卡历史 keyset 游标，
    // 用 (drawn_at, history_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_drawn_at = 0;
    std::string last_history_id;
};

struct GachaHistoryPage {
    std::vector<GachaHistoryEntry> items;
    std::int64_t next_drawn_at = 0;
    std::string next_history_id;
    bool has_more = false;
};

struct GachaDrawResult {
    // GachaDrawResult 是一次单抽或十连的完整结果。
    // grant 记录最终发奖流水，state 是抽完后的最新保底状态。
    GachaPool pool;
    std::vector<GachaHistoryEntry> results;
    GachaPlayerState state;
    std::optional<RewardGrantRecord> grant;
    std::int64_t total_cost = 0;
};

struct GachaDrawTransactionResult {
    // GachaDrawTransactionResult 是抽卡事务仓储的返回结果，
    // 历史、保底状态和奖励流水都在同一个数据库事务内产生。
    std::vector<GachaHistoryEntry> history;
    GachaPlayerState state;
    std::optional<RewardGrantRecord> grant;
    bool currency_not_enough = false;
    bool player_not_found = false;
    bool already_granted = false;
};

struct ShopProduct {
    // ShopProduct 是商城商品配置。
    // UE 只负责展示和发起购买；价格、限购、开放时间和奖励内容都由后端配置决定。
    std::string product_id;
    std::string category = "default";
    std::string name;
    std::string description;
    std::string icon;
    std::string price_currency_id = "gem";
    std::int64_t price_amount = 0;
    std::vector<RewardEntry> rewards;
    std::int64_t purchase_limit = 0;
    bool enabled = true;
    std::optional<std::chrono::system_clock::time_point> starts_at;
    std::optional<std::chrono::system_clock::time_point> ends_at;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct ShopPurchaseRecord {
    // ShopPurchaseRecord 是一次商城购买流水。
    // 客服排查、补偿、消费审计都应该看这张流水，而不是只看当前背包结果。
    std::string purchase_id;
    std::string player_id;
    std::string product_id;
    std::string price_currency_id;
    std::int64_t price_amount = 0;
    std::vector<RewardEntry> rewards;
    std::optional<std::chrono::system_clock::time_point> purchased_at;
};

struct ShopPurchaseResult {
    // ShopPurchaseResult 是购买接口返回给 UE 的完整结果。
    // player 是扣费和发奖后的最新玩家资产，purchase/grant 用于前端展示和问题排查。
    ShopProduct product;
    ShopPurchaseRecord purchase;
    std::optional<RewardGrantRecord> grant;
    PlayerProfile player;
};

struct ShopPurchaseCursor {
    // ShopPurchaseCursor 是购买流水 keyset 游标，
    // 用 (purchased_at, purchase_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_purchased_at = 0;
    std::string last_purchase_id;
};

struct ShopPurchasePage {
    std::vector<ShopPurchaseRecord> items;
    std::int64_t next_purchased_at = 0;
    std::string next_purchase_id;
    bool has_more = false;
};

struct ShopPurchaseTransactionResult {
    // ShopPurchaseTransactionResult 是原子购买仓储的结果。
    // 限购、扣费、发奖和写流水中任何一步失败都会整体回滚，并通过对应标记返回错误原因。
    std::optional<ShopPurchaseRecord> purchase;
    std::optional<RewardGrantRecord> grant;
    std::optional<PlayerProfile> player;
    std::int64_t purchased_count = 0;
    bool limit_reached = false;
    bool currency_not_enough = false;
    bool player_not_found = false;
};

struct LootDrop {
    // LootDrop 是地图/战斗中可拾取掉落的事实记录。
    // 后续可以扩展 scene_id、坐标、队伍归属和距离校验；当前先保留通用字段跑通拾取闭环。
    std::string drop_id;
    std::string owner_player_id;
    std::string scene_id;
    std::vector<RewardEntry> rewards;
    bool picked = false;
    std::string picked_by_player_id;
    std::optional<std::chrono::system_clock::time_point> spawned_at;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::optional<std::chrono::system_clock::time_point> picked_at;
};

struct LootDropCursor {
    // LootDropCursor 是掉落列表 keyset 游标，
    // 用 (spawned_at, drop_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_spawned_at = 0;
    std::string last_drop_id;
};

struct LootDropPage {
    std::vector<LootDrop> items;
    std::int64_t next_spawned_at = 0;
    std::string next_drop_id;
    bool has_more = false;
};

struct LootPickupResult {
    // LootPickupResult 是玩家拾取接口返回给 UE 的完整结果。
    // drop 是已标记拾取后的掉落记录，grant 是奖励流水，player 是最新资产。
    LootDrop drop;
    std::optional<RewardGrantRecord> grant;
    PlayerProfile player;
};

enum class LootPickupStatus {
    picked,
    already_picked_by_self,
    not_found,
    not_owner,
    expired,
    already_picked_by_other,
    player_not_found,
};

struct LootPickupTransactionResult {
    LootPickupStatus status = LootPickupStatus::not_found;
    std::optional<LootPickupResult> value;
};

struct AuditLogEntry {
    // AuditLogEntry 记录后台/GM 的管理操作，方便审计、追责、回放问题现场。
    std::string audit_id;
    std::string operator_id;
    std::string action;
    std::string target_type;
    std::string target_id;
    std::string request_ip;
    std::string user_agent;
    std::string trace_id;
    bool success = true;
    std::string failure_reason;
    std::int64_t duration_ms = 0;
    std::string before_json;
    std::string after_json;
    std::string detail_json;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

struct AuditLogCursor {
    // AuditLogCursor 是后台审计日志 keyset 游标，
    // 用 (created_at, audit_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_created_at = 0;
    std::string last_audit_id;
};

struct AuditLogPage {
    std::vector<AuditLogEntry> items;
    std::int64_t next_created_at = 0;
    std::string next_audit_id;
    bool has_more = false;
};

struct AuditLogWriteRequest {
    // AuditLogWriteRequest 是“准备写一条审计日志”的输入模型。
    // 把上下文、结果和快照字段集中在一起，避免 append() 参数列表不断膨胀。
    std::string operator_id;
    std::string action;
    std::string target_type;
    std::string target_id;
    std::string request_ip;
    std::string user_agent;
    std::string trace_id;
    bool success = true;
    std::string failure_reason;
    std::int64_t duration_ms = 0;
    std::string before_json;
    std::string after_json;
    std::string detail_json;
};

struct AdminPrincipal {
    // AdminPrincipal 表示“这个业务账号在后台系统里是谁”。
    // 它和普通 Account 分开，是为了把管理身份、操作人映射和权限体系独立建模。
    std::string account_id;
    std::string operator_name;
    bool active = true;
    std::vector<std::string> permissions;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

enum class AdminTaskKind {
    announce,
    batch_reward,
    batch_mail,
    batch_mute,
    batch_account_status,
};

struct AdminTask {
    // AdminTask 是后台任务表里的统一任务模型。
    // 第一版先覆盖定时公告和批量任务执行记录，后续可继续扩异步补偿、活动投放等任务。
    std::string task_id;
    AdminTaskKind kind = AdminTaskKind::announce;
    std::string status;
    std::string operator_id;
    std::string target_type;
    std::string target_id;
    std::string request_ip;
    std::string user_agent;
    std::string trace_id;
    std::string request_json;
    std::string result_json;
    std::string failure_reason;
    std::optional<std::chrono::system_clock::time_point> execute_at;
    std::optional<std::chrono::system_clock::time_point> executed_at;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct AdminTaskCursor {
    // AdminTaskCursor 是后台任务列表 keyset 游标，
    // 用 (created_at, task_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_created_at = 0;
    std::string last_task_id;
};

struct AdminTaskPage {
    std::vector<AdminTask> items;
    std::int64_t next_created_at = 0;
    std::string next_task_id;
    bool has_more = false;
};

struct MailMessage {
    // MailMessage 表示玩家收件箱里的单封邮件。
    // 第一版先聚焦标题、正文、附件奖励、已读/已领/已删状态。
    std::string mail_id;
    std::string player_id;
    std::string sender_name;
    std::string title;
    std::string body;
    std::vector<RewardEntry> rewards;
    bool read = false;
    bool claimed = false;
    bool deleted = false;
    std::string claim_grant_id;
    std::optional<std::chrono::system_clock::time_point> sent_at;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::optional<std::chrono::system_clock::time_point> read_at;
    std::optional<std::chrono::system_clock::time_point> claimed_at;
    std::optional<std::chrono::system_clock::time_point> deleted_at;
};

struct FriendLink {
    // FriendLink 故意保持最小化；
    // 生产系统里通常还会扩展 pending、accepted、blocked、来源、时间戳等状态。
    std::string player_id;
    std::string friend_player_id;
    // MySQL 好友分页可以顺便回填昵称；内存仓储未回填时由上层回查。
    std::optional<std::string> friend_nickname;
    // 按最近聊天时间分页时由仓储回填；普通好友列表查询可保持为 0。
    std::int64_t last_sent_at = 0;
};

struct FriendPageCursor {
    // FriendPageCursor 是好友会话列表的 keyset 游标，
    // 用 (last_sent_at, friend_player_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_sent_at = 0;
    std::string last_friend_player_id;
};

struct MailPageCursor {
    // MailPageCursor 是收件箱 keyset 游标，
    // 用 (sent_at, mail_id) 双键避免 OFFSET 深分页扫描。
    std::int64_t last_sent_at = 0;
    std::string last_mail_id;
};

struct FriendRequest {
    // FriendRequest 把“待处理好友关系”从已建立好友中明确拆开，
    // 这样好友申请、同意、拒绝就不需要继续复用 add_friendship 的直加语义。
    std::string requester_player_id;
    std::string target_player_id;
    FriendRequestStatus status = FriendRequestStatus::pending_incoming;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

struct ChatBlockEntry {
    // ChatBlockEntry 表示玩家对另一个玩家的聊天屏蔽关系。
    // 它不等同于好友黑名单，但第一版先聚焦“阻止私聊/互发消息”。
    std::string player_id;
    std::string blocked_player_id;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

struct ChatMuteEntry {
    // ChatMuteEntry 表示频道维度的禁言状态，适用于大厅、房间等多人频道。
    std::string channel_id;
    std::string muted_player_id;
    std::string muted_by_player_id;
    std::string reason;
    std::optional<std::chrono::system_clock::time_point> muted_until;
};

struct ChannelUnreadCount {
    // 第一版未读计数先聚焦到“频道维度”，
    // 这样好友私聊、房间聊天、大厅聊天都能复用同一响应结构。
    std::string channel_id;
    std::int64_t unread_count = 0;
};

struct FriendPresence {
    // FriendPresence 是给好友列表、私聊面板、邀请面板复用的轻量在线态结构。
    std::string player_id;
    bool online = false;
    std::int64_t connection_count = 0;
};

struct Session {
    // Session 把 bearer token 鉴权从 HTTP 或 WebSocket 细节里抽离出来。
    std::string session_id;
    std::string token;
    std::string refresh_token;
    std::string account_id;
    std::string player_id;
    std::string nickname;
    std::string client_ip;
    std::string user_agent;
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point expires_at;
};

struct PasswordResetTicket {
    // PasswordResetTicket 是“忘记密码”流程里的中间凭据。
    // 当前示例会直接把 reset_token 返回给调用方，方便联调；
    // 生产环境里应改成发邮件/短信，而不是把 token 回给客户端。
    std::string account_id;
    std::string login_name;
    std::string reset_token;
    std::chrono::system_clock::time_point expires_at;
};

struct CaptchaChallenge {
    // 当前先做一个可 review 的轻量挑战题模型，
    // 让登录风控能真正要求客户端先做人机校验，而不是只留 TODO。
    std::string challenge_id;
    std::string prompt;
    std::chrono::system_clock::time_point expires_at;
};

struct LoginRiskState {
    bool blacklisted = false;
    bool captcha_required = false;
    bool rate_limited = false;
    std::int64_t failure_count = 0;
    std::int64_t retry_after_seconds = 0;
};

struct BlacklistEntry {
    std::string entry_id;
    std::string subject_type;
    std::string subject_value;
    std::string reason;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

struct ChatChannel {
    // ChatChannel 把“频道是什么”明确建模出来，
    // 这样大厅、局内房间、好友私聊就不再只是一个随意拼接的字符串。
    std::string channel_id;
    ChatChannelKind kind = ChatChannelKind::lobby;
    std::string scope_id;
    std::string display_name;
};

struct AnnouncementEntry {
    // AnnouncementEntry 是独立于聊天消息的“公告事实模型”。
    // 它专门服务公告列表、启动弹窗、后台回查，不依赖频道里历史消息是否还保留在最近窗口。
    // 它放在 ChatChannel 后面，是因为公告本身要直接携带频道信息。
    std::string announcement_id;
    ChatChannel channel;
    std::string operator_id;
    std::string title;
    std::string body;
    std::optional<std::chrono::system_clock::time_point> published_at;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

struct ChatMessage {
    // ChatMessage 现在不仅知道发到哪里，还知道这是哪种消息类型，
    // 后续要扩系统消息、GM 公告、表情消息会更自然。
    std::string message_id;
    ChatChannel channel;
    ChatMessageKind kind = ChatMessageKind::text;
    std::string sender_player_id;
    std::string sender_nickname;
    std::string text;
    bool recalled = false;
    std::string recalled_by_player_id;
    std::optional<std::chrono::system_clock::time_point> recalled_at;
    std::chrono::system_clock::time_point sent_at;
};

}  // namespace mf::game 命名空间
