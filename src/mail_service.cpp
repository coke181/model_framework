#include "mf/game/mail_service.hpp"
#include "mf/core/profiling.hpp"

#include <algorithm>
#include <unordered_set>

// 文件职责：
// 实现邮件和奖励发放的业务编排。
// 调用流程：
// HTTP 层传入玩家、邮件、奖励参数 ->
// 这里负责校验、幂等、发奖 ->
// 最后再返回聚合后的邮件/奖励/玩家结果。

namespace mf::game {

namespace {

bool is_visible_mail_for_player(const MailMessage& mail, bool include_deleted) {
    // UE 收件箱必须只返回可操作邮件。
    // mail_id 为空的历史脏数据无法 read/claim/delete，直接过滤，避免客户端显示“空邮件”。
    if (mail.mail_id.empty() || mail.player_id.empty()) {
        return false;
    }
    if (!include_deleted && mail.deleted) {
        return false;
    }
    return true;
}

}  // namespace

// 构造 RewardService，注入“玩家资产写入”和“奖励流水记录”两个依赖。
RewardService::RewardService(
    std::shared_ptr<PlayerRepository> players,
    std::shared_ptr<RewardRepository> rewards,
    std::shared_ptr<RewardTemplateRepository> reward_templates,
    std::shared_ptr<ItemCatalogRepository> item_catalog,
    std::shared_ptr<RewardTransactionRepository> reward_transactions)
    : players_(std::move(players)),
      rewards_(std::move(rewards)),
      reward_templates_(std::move(reward_templates)),
      item_catalog_(std::move(item_catalog)),
      reward_transactions_(std::move(reward_transactions)) {}

// 直接给指定玩家发奖励。
// 这里会先做空奖励和玩家存在校验，再用 grant_id 做幂等保护，
// 最后先改玩家资产，再记录奖励流水。
ServiceResult<RewardGrantRecord> RewardService::grant_to_player(
    const std::string& player_id,
    const std::string& grant_id,
    std::string source_type,
    std::string source_id,
    std::vector<RewardEntry> reward_items) {
    if (reward_items.empty()) {
        return ServiceResult<RewardGrantRecord>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "reward_empty",
                .message = "奖励内容不能为空",
            },
        };
    }

    if (!players_->find_by_player_id(player_id)) {
        return ServiceResult<RewardGrantRecord>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "player_not_found",
                .message = "目标玩家不存在",
            },
        };
    }

    auto expanded_rewards = expand_reward_templates(reward_items);
    if (expanded_rewards.empty()) {
        return ServiceResult<RewardGrantRecord>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "reward_empty_after_expand",
                .message = "奖励模板展开后没有有效奖励内容",
            },
        };
    }

    // 自动补齐物品配置表：
    // 拾取、邮件、商城、抽卡最终都会走 RewardService。
    // 如果奖励里出现 item，但 item_definition 还没有配置，就先写入一条默认配置，
    // 避免玩家背包里出现“有 item_id，但后台物品配置表查不到”的半成品数据。
    ensure_item_definitions_for_rewards(expanded_rewards);

    // 真实数据库模式使用原子事务仓储，确保奖励流水和玩家资产一起提交或一起回滚。
    if (reward_transactions_) {
        auto transaction_result = reward_transactions_->grant_once(
            player_id,
            grant_id,
            source_type,
            source_id,
            expanded_rewards);
        if (!transaction_result.grant) {
            return ServiceResult<RewardGrantRecord>{
                .error = mf::core::ApiError{
                    .status = boost::beast::http::status::not_found,
                    .code = "player_not_found",
                    .message = "目标玩家不存在",
                },
            };
        }
        return ServiceResult<RewardGrantRecord>{.value = std::move(transaction_result.grant)};
    }

    if (const auto existing = rewards_->find_by_grant_id(grant_id)) {
        return ServiceResult<RewardGrantRecord>{.value = existing};
    }

    const auto updated_player = players_->apply_rewards(player_id, expanded_rewards, grant_id);
    if (!updated_player) {
        return ServiceResult<RewardGrantRecord>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "player_not_found",
                .message = "目标玩家不存在",
            },
        };
    }

    return ServiceResult<RewardGrantRecord>{
        .value = rewards_->record_grant(
            grant_id,
            player_id,
            std::move(source_type),
            std::move(source_id),
            std::move(expanded_rewards)),
    };
}

// 查询玩家最近奖励流水，方便客户端或 GM 面板做奖励追踪。
std::vector<RewardGrantRecord> RewardService::list_grants(const std::string& player_id, std::size_t limit) const {
    return rewards_->list_by_player_id(player_id, limit);
}

// 后台奖励流水查询直接走仓储层筛选，
// 避免先把大结果集拉回进程内再做二次过滤。
std::size_t RewardService::count_grants_filtered(
    const std::string& player_id,
    const std::optional<std::string>& source_type_filter,
    const std::optional<std::string>& source_id_filter) const {
    return rewards_->count_filtered(player_id, source_type_filter, source_id_filter);
}

// 这个接口给后台列表页使用，
// 和 list_grants 不同，它会把分页与筛选一起下推到底层仓储。
std::vector<RewardGrantRecord> RewardService::list_grants_filtered(
    const std::string& player_id,
    std::size_t limit,
    std::size_t offset,
    const std::optional<std::string>& source_type_filter,
    const std::optional<std::string>& source_id_filter) const {
    return rewards_->list_filtered(player_id, limit, offset, source_type_filter, source_id_filter);
}

std::vector<RewardEntry> RewardService::expand_reward_templates(const std::vector<RewardEntry>& reward_items) const {
    std::vector<RewardEntry> expanded;
    for (const auto& reward : reward_items) {
        if (reward.amount <= 0) {
            continue;
        }

        if (reward.kind != RewardKind::attachment_template) {
            expanded.push_back(reward);
            continue;
        }

        const auto reward_template = reward_templates_->find_by_template_id(reward.resource_id);
        if (!reward_template) {
            continue;
        }

        for (std::int64_t repeat = 0; repeat < reward.amount; ++repeat) {
            const auto nested = expand_reward_templates(reward_template->rewards);
            expanded.insert(expanded.end(), nested.begin(), nested.end());
        }
    }
    return expanded;
}

void RewardService::ensure_item_definitions_for_rewards(const std::vector<RewardEntry>& reward_items) const {
    if (!item_catalog_) {
        return;
    }

    std::unordered_set<std::string> checked_item_ids;
    for (const auto& reward : reward_items) {
        if (reward.kind != RewardKind::item || reward.amount <= 0 || reward.resource_id.empty()) {
            continue;
        }
        if (!checked_item_ids.insert(reward.resource_id).second) {
            continue;
        }
        if (item_catalog_->find_by_item_id(reward.resource_id)) {
            continue;
        }

        item_catalog_->upsert_item(ItemDefinition{
            .item_id = reward.resource_id,
            .name = reward.resource_id,
            .description = "自动收集到的物品，请在后台物品配置页补充正式描述",
            .icon = "",
            .rarity = "3星",
            .max_stack = 9999,
            .item_type = "auto_collected",
            .enabled = true,
        });
    }
}

// 构造 MailService，注入邮件仓储、玩家仓储、奖励仓储和发奖服务。
MailService::MailService(
    std::shared_ptr<MailRepository> mails,
    std::shared_ptr<PlayerRepository> players,
    std::shared_ptr<RewardRepository> rewards,
    std::shared_ptr<RewardService> reward_service)
    : mails_(std::move(mails)),
      players_(std::move(players)),
      rewards_(std::move(rewards)),
      reward_service_(std::move(reward_service)) {}

// 给单个玩家发送一封系统邮件。
// 这一步只负责生成收件箱邮件事实，不直接发放附件。
MailMessage MailService::send_mail(
    const std::string& player_id,
    std::string sender_name,
    std::string title,
    std::string body,
    std::vector<RewardEntry> rewards,
    std::optional<std::chrono::system_clock::time_point> expires_at) {
    MF_PROFILE_SCOPE("Mail send");
    return mails_->send_mail(MailMessage{
        .player_id = player_id,
        .sender_name = std::move(sender_name),
        .title = std::move(title),
        .body = std::move(body),
        .rewards = std::move(rewards),
        .sent_at = std::chrono::system_clock::now(),
        .expires_at = expires_at,
    });
}

// 批量发送系统邮件。
// 第一版对不存在的玩家直接跳过，避免一次批量投递因为单个坏玩家中断。
std::vector<MailMessage> MailService::send_batch(
    const std::vector<std::string>& player_ids,
    const std::string& sender_name,
    const std::string& title,
    const std::string& body,
    const std::vector<RewardEntry>& rewards,
    std::optional<std::chrono::system_clock::time_point> expires_at) {
    std::vector<MailMessage> items;
    items.reserve(player_ids.size());
    for (const auto& player_id : player_ids) {
        if (!players_->find_by_player_id(player_id)) {
            continue;
        }
        items.push_back(send_mail(
            player_id,
            sender_name,
            title,
            body,
            rewards,
            expires_at));
    }
    return items;
}

// 拉取玩家收件箱列表。
// include_deleted=true 时，方便 GM 或调试场景连逻辑删除邮件也一起看见。
std::vector<MailMessage> MailService::list_inbox(const std::string& player_id, bool include_deleted) const {
    MF_PROFILE_SCOPE("Mail list inbox");
    auto items = mails_->list_inbox(player_id, include_deleted);
    items.erase(
        std::remove_if(items.begin(), items.end(), [&](const MailMessage& mail) {
            return !is_visible_mail_for_player(mail, include_deleted);
        }),
        items.end());
    return items;
}

std::vector<MailMessage> MailService::list_inbox_cursor(
    const std::string& player_id,
    bool include_deleted,
    std::size_t limit,
    const std::optional<MailPageCursor>& cursor) const {
    MF_PROFILE_SCOPE("Mail list inbox cursor");
    auto items = mails_->list_inbox_cursor(player_id, include_deleted, limit, cursor);
    items.erase(
        std::remove_if(items.begin(), items.end(), [&](const MailMessage& mail) {
            return !is_visible_mail_for_player(mail, include_deleted);
        }),
        items.end());
    return items;
}

// 后台邮件列表页要拿精确总数，
// 这里单独暴露 count，路由层就不需要自己扫整页数据了。
std::size_t MailService::count_inbox_filtered(
    const std::string& player_id,
    bool include_deleted,
    const std::optional<bool>& claimed_filter,
    const std::optional<bool>& read_filter) const {
    return mails_->count_filtered(player_id, include_deleted, claimed_filter, read_filter);
}

// 后台邮件筛选查询统一从这里进，
// 由仓储层决定如何把条件翻译成内存过滤或 SQL WHERE。
std::vector<MailMessage> MailService::list_inbox_filtered(
    const std::string& player_id,
    bool include_deleted,
    std::size_t limit,
    std::size_t offset,
    const std::optional<bool>& claimed_filter,
    const std::optional<bool>& read_filter) const {
    auto items = mails_->list_filtered(player_id, include_deleted, limit, offset, claimed_filter, read_filter);
    items.erase(
        std::remove_if(items.begin(), items.end(), [&](const MailMessage& mail) {
            return !is_visible_mail_for_player(mail, include_deleted);
        }),
        items.end());
    return items;
}

// 标记邮件为已读。
// 这里只更新邮件状态，不触发附件奖励发放。
ServiceResult<MailMessage> MailService::mark_read(const std::string& player_id, const std::string& mail_id) {
    const auto mail = mails_->mark_read(player_id, mail_id, std::chrono::system_clock::now());
    if (!mail) {
        return ServiceResult<MailMessage>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "mail_not_found",
                .message = "邮件不存在",
            },
        };
    }
    return ServiceResult<MailMessage>{.value = mail};
}

// 领取邮件附件。
// 这是邮件模块最核心的跨仓储流程：先校验邮件，再通过 RewardService 发奖，
// 最后把邮件标记成已领取，并返回邮件、奖励流水、玩家最新资产三个视图。
ServiceResult<MailService::ClaimResult> MailService::claim_mail(
    const std::string& player_id,
    const std::string& mail_id) {
    const auto mail = mails_->find_mail(player_id, mail_id);
    if (!mail || mail->deleted) {
        return ServiceResult<ClaimResult>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "mail_not_found",
                .message = "邮件不存在",
            },
        };
    }

    const auto now = std::chrono::system_clock::now();
    if (mail->expires_at && *mail->expires_at < now) {
        return ServiceResult<ClaimResult>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::gone,
                .code = "mail_expired",
                .message = "邮件已过期",
            },
        };
    }

    const auto grant_id = "mail_claim:" + mail_id;
    if (mail->claimed) {
        if (const auto existing_grant = rewards_->find_by_grant_id(grant_id)) {
            const auto player = players_->find_by_player_id(player_id).value_or(PlayerProfile{});
            return ServiceResult<ClaimResult>{
                .value = ClaimResult{
                    .mail = *mail,
                    .grant = *existing_grant,
                    .player = player,
                },
            };
        }
    }

    auto grant_result = reward_service_->grant_to_player(
        player_id,
        grant_id,
        "mail",
        mail_id,
        mail->rewards);
    if (grant_result.error) {
        return ServiceResult<ClaimResult>{
            .value = std::nullopt,
            .error = grant_result.error,
        };
    }

    const auto claimed_mail = mails_->mark_claimed(player_id, mail_id, grant_id, now);
    const auto player = players_->find_by_player_id(player_id);
    if (!claimed_mail || !player) {
        return ServiceResult<ClaimResult>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::internal_server_error,
                .code = "mail_claim_state_invalid",
                .message = "邮件领取状态保存失败",
            },
        };
    }

    return ServiceResult<ClaimResult>{
        .value = ClaimResult{
            .mail = *claimed_mail,
            .grant = *grant_result.value,
            .player = *player,
        },
    };
}

// 删除邮件。
// 如果邮件还有未领取附件，这里先执行一次幂等领取，再把邮件标记删除。
// 这样 UE 侧只需要点“删除邮件”一个按钮，不会因为忘记先点“领取附件”而卡住。
ServiceResult<MailMessage> MailService::delete_mail(const std::string& player_id, const std::string& mail_id) {
    if (mail_id.empty()) {
        return ServiceResult<MailMessage>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "mail_id_required",
                .message = "mail_id 不能为空",
            },
        };
    }

    const auto mail = mails_->find_mail(player_id, mail_id);
    if (!mail) {
        return ServiceResult<MailMessage>{
            .value = std::nullopt,
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "mail_not_found",
                .message = "邮件不存在",
            },
        };
    }

    if (mail->deleted) {
        return ServiceResult<MailMessage>{.value = mail};
    }

    if (!mail->claimed && !mail->rewards.empty()) {
        const auto claim_result = claim_mail(player_id, mail_id);
        if (claim_result.error) {
            // 历史坏邮件可能存在“显示有附件，但奖励内容无法正常入账”的情况。
            // 删除动作不能因为这类坏附件永久失败；这里允许继续逻辑删除，让玩家邮箱恢复可用。
            return ServiceResult<MailMessage>{
                .value = mails_->mark_deleted(player_id, mail_id, std::chrono::system_clock::now()),
            };
        }
    }

    return ServiceResult<MailMessage>{
        .value = mails_->mark_deleted(player_id, mail_id, std::chrono::system_clock::now()),
    };
}

}  // namespace mf::game 命名空间
