#pragma once

// 文件职责：
// 封装邮件与奖励发放的跨仓储业务流程。
// 调用流程：
// HTTP handler 调用 MailService / RewardService ->
// 服务层先做幂等和业务校验 ->
// 再协调 PlayerRepository、RewardRepository、MailRepository 完成真实落库与返回。

#include "mf/core/api.hpp"
#include "mf/game/models.hpp"
#include "mf/game/repositories.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mf::game {

template <typename T>
struct ServiceResult {
    std::optional<T> value;
    std::optional<mf::core::ApiError> error;
};

class RewardService {
public:
    // RewardService 负责直接发奖的通用流程。
    // 后续邮件领取、活动结算、商城发货都可以复用它。
    RewardService(
        std::shared_ptr<PlayerRepository> players,
        std::shared_ptr<RewardRepository> rewards,
        std::shared_ptr<RewardTemplateRepository> reward_templates,
        std::shared_ptr<ItemCatalogRepository> item_catalog = {},
        std::shared_ptr<RewardTransactionRepository> reward_transactions = {});

    ServiceResult<RewardGrantRecord> grant_to_player(
        const std::string& player_id,
        const std::string& grant_id,
        std::string source_type,
        std::string source_id,
        std::vector<RewardEntry> reward_items);
    std::vector<RewardGrantRecord> list_grants(const std::string& player_id, std::size_t limit) const;
    std::size_t count_grants_filtered(
        const std::string& player_id,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const;
    std::vector<RewardGrantRecord> list_grants_filtered(
        const std::string& player_id,
        std::size_t limit,
        std::size_t offset,
        const std::optional<std::string>& source_type_filter,
        const std::optional<std::string>& source_id_filter) const;

private:
    std::vector<RewardEntry> expand_reward_templates(const std::vector<RewardEntry>& reward_items) const;
    void ensure_item_definitions_for_rewards(const std::vector<RewardEntry>& reward_items) const;

    std::shared_ptr<PlayerRepository> players_;
    std::shared_ptr<RewardRepository> rewards_;
    std::shared_ptr<RewardTemplateRepository> reward_templates_;
    std::shared_ptr<ItemCatalogRepository> item_catalog_;
    std::shared_ptr<RewardTransactionRepository> reward_transactions_;
};

class MailService {
public:
    struct ClaimResult {
        MailMessage mail;
        RewardGrantRecord grant;
        PlayerProfile player;
    };

    MailService(
        std::shared_ptr<MailRepository> mails,
        std::shared_ptr<PlayerRepository> players,
        std::shared_ptr<RewardRepository> rewards,
        std::shared_ptr<RewardService> reward_service);

    MailMessage send_mail(
        const std::string& player_id,
        std::string sender_name,
        std::string title,
        std::string body,
        std::vector<RewardEntry> rewards,
        std::optional<std::chrono::system_clock::time_point> expires_at);
    std::vector<MailMessage> send_batch(
        const std::vector<std::string>& player_ids,
        const std::string& sender_name,
        const std::string& title,
        const std::string& body,
        const std::vector<RewardEntry>& rewards,
        std::optional<std::chrono::system_clock::time_point> expires_at);

    std::vector<MailMessage> list_inbox(const std::string& player_id, bool include_deleted) const;
    std::vector<MailMessage> list_inbox_cursor(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        const std::optional<MailPageCursor>& cursor) const;
    std::size_t count_inbox_filtered(
        const std::string& player_id,
        bool include_deleted,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const;
    std::vector<MailMessage> list_inbox_filtered(
        const std::string& player_id,
        bool include_deleted,
        std::size_t limit,
        std::size_t offset,
        const std::optional<bool>& claimed_filter,
        const std::optional<bool>& read_filter) const;
    ServiceResult<MailMessage> mark_read(const std::string& player_id, const std::string& mail_id);
    ServiceResult<ClaimResult> claim_mail(const std::string& player_id, const std::string& mail_id);
    ServiceResult<MailMessage> delete_mail(const std::string& player_id, const std::string& mail_id);

private:
    std::shared_ptr<MailRepository> mails_;
    std::shared_ptr<PlayerRepository> players_;
    std::shared_ptr<RewardRepository> rewards_;
    std::shared_ptr<RewardService> reward_service_;
};

}  // namespace mf::game 命名空间
