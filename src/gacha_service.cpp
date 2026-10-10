#include "mf/game/gacha_service.hpp"

// 文件职责：
// 实现抽卡服务的业务闭环。
// 调用流程：
// HTTP /gacha/draw -> GachaService::draw ->
// 读取卡池配置和玩家保底 -> 扣除货币 -> 随机抽取奖品 ->
// 更新保底和历史 -> 通过 RewardService 发奖。

#include <algorithm>
#include <chrono>
#include <sstream>

namespace mf::game {
namespace {

// 生成抽卡历史 ID。
// 这里用玩家、卡池、当前时间和序号组合，保证同一次十连里的每条记录都有稳定 ID。
std::string make_gacha_history_id(const std::string& player_id, const std::string& pool_id, std::int64_t index) {
    const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::ostringstream output;
    output << "gacha_" << player_id << '_' << pool_id << '_' << tick << '_' << index;
    return output.str();
}

// 判断卡池是否在开放时间内。
// starts_at / ends_at 为空表示不限制对应边界。
bool pool_is_open(const GachaPool& pool, std::chrono::system_clock::time_point now) {
    if (!pool.enabled) {
        return false;
    }
    if (pool.starts_at && now < *pool.starts_at) {
        return false;
    }
    if (pool.ends_at && now > *pool.ends_at) {
        return false;
    }
    return true;
}

}  // 匿名命名空间

// 构造抽卡服务，注入抽卡仓储、玩家仓储和奖励服务。
GachaService::GachaService(
    std::shared_ptr<GachaRepository> gacha,
    std::shared_ptr<PlayerRepository> players,
    std::shared_ptr<RewardService> rewards,
    std::shared_ptr<GachaTransactionRepository> gacha_transactions)
    : gacha_(std::move(gacha)),
      players_(std::move(players)),
      rewards_(std::move(rewards)),
      gacha_transactions_(std::move(gacha_transactions)),
      random_(std::random_device{}()) {}

// 执行抽卡主流程。
// 注意：这里先扣费再发奖；如果后续要做金融级一致性，应把扣费、历史、发奖升级为同库事务或补偿任务。
ServiceResult<GachaDrawResult> GachaService::draw(
    const std::string& player_id,
    const std::string& pool_id,
    std::int64_t count) {
    if (count != 1 && count != 10) {
        return ServiceResult<GachaDrawResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "invalid_gacha_count",
                .message = "抽卡次数只能是 1 或 10",
            },
        };
    }

    const auto pool = gacha_->find_pool(pool_id);
    if (!pool) {
        return ServiceResult<GachaDrawResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "gacha_pool_not_found",
                .message = "卡池不存在",
            },
        };
    }

    if (!pool_is_open(*pool, std::chrono::system_clock::now())) {
        return ServiceResult<GachaDrawResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::forbidden,
                .code = "gacha_pool_closed",
                .message = "卡池未开放或已关闭",
            },
        };
    }

    const auto items = gacha_->list_pool_items(pool_id);
    if (items.empty()) {
        return ServiceResult<GachaDrawResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "gacha_pool_empty",
                .message = "卡池没有配置任何奖品",
            },
        };
    }

    const auto total_cost = pool->cost_amount_per_draw * count;
    if (gacha_transactions_) {
        std::vector<std::string> history_ids;
        history_ids.reserve(static_cast<std::size_t>(count));
        for (std::int64_t index = 0; index < count; ++index) {
            history_ids.push_back(make_gacha_history_id(player_id, pool_id, index));
        }
        const auto grant_id = make_gacha_history_id(player_id, pool_id, count);
        auto transaction = gacha_transactions_->draw_once(
            player_id,
            pool_id,
            *pool,
            items,
            count,
            total_cost,
            history_ids,
            grant_id,
            std::chrono::system_clock::now(),
            [this, &pool, &items](const GachaPlayerState& state) {
                return pick_item(*pool, items, state);
            });

        if (transaction.currency_not_enough) {
            return ServiceResult<GachaDrawResult>{
                .error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "currency_not_enough",
                    .message = "货币不足，无法抽卡",
                    .details = {
                        {"currency_id", pool->cost_currency_id},
                        {"required", total_cost},
                    },
                },
            };
        }
        if (transaction.player_not_found) {
            return ServiceResult<GachaDrawResult>{
                .error = mf::core::ApiError{
                    .status = boost::beast::http::status::not_found,
                    .code = "player_not_found",
                    .message = "玩家不存在",
                },
            };
        }

        return ServiceResult<GachaDrawResult>{
            .value = GachaDrawResult{
                .pool = *pool,
                .results = std::move(transaction.history),
                .state = std::move(transaction.state),
                .grant = std::move(transaction.grant),
                .total_cost = total_cost,
            },
        };
    }

    if (total_cost > 0) {
        const auto spent = players_->spend_currency(player_id, pool->cost_currency_id, total_cost);
        if (!spent) {
            return ServiceResult<GachaDrawResult>{
                .error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "currency_not_enough",
                    .message = "货币不足，无法抽卡",
                    .details = {
                        {"currency_id", pool->cost_currency_id},
                        {"required", total_cost},
                    },
                },
            };
        }
    }

    auto state = gacha_->get_player_state(player_id, pool_id);
    std::vector<GachaHistoryEntry> history;
    std::vector<RewardEntry> rewards;
    history.reserve(static_cast<std::size_t>(count));
    rewards.reserve(static_cast<std::size_t>(count));

    for (std::int64_t index = 0; index < count; ++index) {
        const auto pity_5_before = state.pity_5_count;
        const auto pity_4_before = state.pity_4_count;
        const auto item = pick_item(*pool, items, state);

        if (item.rarity >= 5) {
            state.pity_5_count = 0;
            state.pity_4_count = 0;
        } else if (item.rarity >= 4) {
            ++state.pity_5_count;
            state.pity_4_count = 0;
        } else {
            ++state.pity_5_count;
            ++state.pity_4_count;
        }
        ++state.total_draws;

        history.push_back(GachaHistoryEntry{
            .history_id = make_gacha_history_id(player_id, pool_id, index),
            .player_id = player_id,
            .pool_id = pool_id,
            .item_key = item.item_key,
            .rarity = item.rarity,
            .is_up = item.is_up,
            .reward = item.reward,
            .pity_5_before = pity_5_before,
            .pity_4_before = pity_4_before,
            .drawn_at = std::chrono::system_clock::now(),
        });
        rewards.push_back(item.reward);
    }

    state = gacha_->save_player_state(state);
    gacha_->append_history(history);

    const auto grant_id = make_gacha_history_id(player_id, pool_id, count);
    const auto grant = rewards_->grant_to_player(player_id, grant_id, "gacha", pool_id, rewards);
    if (grant.error) {
        return ServiceResult<GachaDrawResult>{.error = grant.error};
    }

    return ServiceResult<GachaDrawResult>{
        .value = GachaDrawResult{
            .pool = *pool,
            .results = std::move(history),
            .state = std::move(state),
            .grant = grant.value,
            .total_cost = total_cost,
        },
    };
}

// 按保底和权重选择一个奖品。
// 先判断是否触发 5 星/4 星硬保底，再在符合稀有度条件的奖品里按 weight 加权随机。
GachaPoolItem GachaService::pick_item(
    const GachaPool& pool,
    const std::vector<GachaPoolItem>& items,
    const GachaPlayerState& state) {
    const auto force_5 = pool.pity_5 > 0 && state.pity_5_count + 1 >= pool.pity_5;
    const auto force_4_or_above = !force_5 && pool.pity_4 > 0 && state.pity_4_count + 1 >= pool.pity_4;

    std::vector<GachaPoolItem> candidates;
    for (const auto& item : items) {
        if (item.weight <= 0) {
            continue;
        }
        if (force_5 && item.rarity < 5) {
            continue;
        }
        if (force_4_or_above && item.rarity < 4) {
            continue;
        }
        candidates.push_back(item);
    }
    if (candidates.empty()) {
        candidates = items;
    }

    std::int64_t total_weight = 0;
    for (const auto& item : candidates) {
        total_weight += std::max<std::int64_t>(1, item.weight);
    }
    std::uniform_int_distribution<std::int64_t> distribution(1, std::max<std::int64_t>(1, total_weight));
    auto ticket = distribution(random_);
    for (const auto& item : candidates) {
        ticket -= std::max<std::int64_t>(1, item.weight);
        if (ticket <= 0) {
            return item;
        }
    }
    return candidates.back();
}

}  // namespace mf::game 命名空间
