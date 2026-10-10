#include "mf/game/loot_service.hpp"

// 文件职责：
// 实现掉落生成和拾取发奖闭环。
// 调用流程：
// /admin/loot/spawn 生成掉落 -> /loot/pickup 校验并标记拾取 -> RewardService 发奖。

#include <boost/beast/http/status.hpp>

#include <chrono>
#include <sstream>
#include <utility>

namespace mf::game {
namespace {

// 生成掉落 ID。
// 当前使用玩家、场景和微秒时间组合，后续可以替换为全局 ID 服务。
std::string make_drop_id(const std::string& player_id, const std::string& scene_id) {
    const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::ostringstream output;
    output << "drop_" << player_id << '_' << scene_id << '_' << tick;
    return output.str();
}

}  // 匿名命名空间

// 构造掉落服务，注入掉落仓储、玩家仓储和奖励服务。
LootService::LootService(
    std::shared_ptr<LootRepository> loot,
    std::shared_ptr<PlayerRepository> players,
    std::shared_ptr<RewardService> rewards,
    std::shared_ptr<LootTransactionRepository> loot_transactions)
    : loot_(std::move(loot)),
      players_(std::move(players)),
      rewards_(std::move(rewards)),
      loot_transactions_(std::move(loot_transactions)) {}

// 生成一个玩家归属掉落。
// ttl<=0 表示不过期；rewards 不能为空，否则生成出来也无法拾取。
ServiceResult<LootDrop> LootService::spawn_for_player(
    std::string owner_player_id,
    std::string scene_id,
    std::vector<RewardEntry> rewards,
    std::chrono::seconds ttl) {
    if (owner_player_id.empty()) {
        return ServiceResult<LootDrop>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "loot_owner_required",
                .message = "owner_player_id 不能为空",
            },
        };
    }
    if (!players_->find_by_player_id(owner_player_id)) {
        return ServiceResult<LootDrop>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "player_not_found",
                .message = "玩家不存在，无法生成掉落",
            },
        };
    }
    if (rewards.empty()) {
        return ServiceResult<LootDrop>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "loot_rewards_empty",
                .message = "掉落奖励不能为空",
            },
        };
    }

    const auto now = std::chrono::system_clock::now();
    if (scene_id.empty()) {
        scene_id = "default";
    }
    auto drop = loot_->create_drop(LootDrop{
        .drop_id = make_drop_id(owner_player_id, scene_id),
        .owner_player_id = std::move(owner_player_id),
        .scene_id = std::move(scene_id),
        .rewards = std::move(rewards),
        .spawned_at = now,
        .expires_at = ttl.count() > 0 ? std::optional<std::chrono::system_clock::time_point>{now + ttl} : std::nullopt,
    });
    return ServiceResult<LootDrop>{.value = std::move(drop)};
}

// 拾取一个掉落。
// 先检查状态，再调用 mark_picked 做最终状态切换；如果其他请求抢先拾取，mark_picked 会失败。
ServiceResult<LootPickupResult> LootService::pickup(const std::string& player_id, const std::string& drop_id) {
    if (drop_id.empty()) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "loot_drop_id_required",
                .message = "drop_id 不能为空",
            },
        };
    }

    // MySQL 模式在一个事务中完成掉落锁定、发奖、资产更新和拾取标记。
    if (loot_transactions_) {
        auto transaction_result = loot_transactions_->pickup_once(
            player_id,
            drop_id,
            std::chrono::system_clock::now());
        if (transaction_result.value) {
            return ServiceResult<LootPickupResult>{.value = std::move(transaction_result.value)};
        }

        mf::core::ApiError error;
        switch (transaction_result.status) {
        case LootPickupStatus::not_found:
            error = {.status = boost::beast::http::status::not_found, .code = "loot_not_found", .message = "掉落不存在"};
            break;
        case LootPickupStatus::not_owner:
            error = {.status = boost::beast::http::status::forbidden, .code = "loot_not_owner", .message = "该掉落不属于当前玩家"};
            break;
        case LootPickupStatus::expired:
            error = {.status = boost::beast::http::status::gone, .code = "loot_expired", .message = "掉落已经过期"};
            break;
        case LootPickupStatus::already_picked_by_other:
            error = {.status = boost::beast::http::status::conflict, .code = "loot_already_picked", .message = "掉落已经被其他玩家拾取"};
            break;
        case LootPickupStatus::player_not_found:
            error = {.status = boost::beast::http::status::not_found, .code = "player_not_found", .message = "玩家不存在"};
            break;
        default:
            error = {.status = boost::beast::http::status::conflict, .code = "loot_pickup_failed", .message = "拾取失败"};
            break;
        }
        return ServiceResult<LootPickupResult>{.error = std::move(error)};
    }

    const auto before = loot_->find_drop(drop_id);
    if (!before) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "loot_not_found",
                .message = "掉落不存在",
            },
        };
    }
    if (before->picked) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::conflict,
                .code = "loot_already_picked",
                .message = "该掉落已经被拾取",
            },
        };
    }
    if (!before->owner_player_id.empty() && before->owner_player_id != player_id) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::forbidden,
                .code = "loot_not_owner",
                .message = "该掉落不属于当前玩家",
            },
        };
    }
    const auto now = std::chrono::system_clock::now();
    if (before->expires_at && now > *before->expires_at) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::gone,
                .code = "loot_expired",
                .message = "掉落已经过期",
            },
        };
    }
    if (before->rewards.empty()) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "loot_rewards_empty",
                .message = "掉落没有配置奖励，无法拾取",
            },
        };
    }

    const auto picked = loot_->mark_picked(drop_id, player_id, now);
    if (!picked) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::conflict,
                .code = "loot_already_picked",
                .message = "该掉落已经被其他请求拾取",
            },
        };
    }

    const auto grant = rewards_->grant_to_player(player_id, "loot_" + drop_id, "loot", drop_id, picked->rewards);
    if (grant.error) {
        return ServiceResult<LootPickupResult>{.error = grant.error};
    }

    const auto player = players_->find_by_player_id(player_id);
    if (!player) {
        return ServiceResult<LootPickupResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "player_not_found",
                .message = "玩家不存在，无法返回拾取结果",
            },
        };
    }

    return ServiceResult<LootPickupResult>{
        .value = LootPickupResult{
            .drop = *picked,
            .grant = grant.value,
            .player = *player,
        },
    };
}

}  // namespace mf::game 命名空间
