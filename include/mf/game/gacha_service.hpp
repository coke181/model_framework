#pragma once

// 文件职责：
// 封装抽卡业务规则，避免 HTTP handler 直接操作卡池表、玩家资产和奖励流水。
// 调用流程：
// 玩家/后台 HTTP 路由 -> GachaService ->
// GachaRepository 读取卡池和保底 -> PlayerRepository 扣货币 ->
// RewardService 发奖 -> GachaRepository 保存历史和最新保底。

#include "mf/game/mail_service.hpp"
#include "mf/game/models.hpp"
#include "mf/game/repositories.hpp"

#include <memory>
#include <random>
#include <string>

namespace mf::game {

class GachaService {
public:
    GachaService(
        std::shared_ptr<GachaRepository> gacha,
        std::shared_ptr<PlayerRepository> players,
        std::shared_ptr<RewardService> rewards,
        std::shared_ptr<GachaTransactionRepository> gacha_transactions = {});

    // draw 执行一次单抽或十连。
    // 它是抽卡的唯一入口：扣费、随机、保底、发奖、记历史都集中在这里完成。
    ServiceResult<GachaDrawResult> draw(
        const std::string& player_id,
        const std::string& pool_id,
        std::int64_t count);

private:
    GachaPoolItem pick_item(
        const GachaPool& pool,
        const std::vector<GachaPoolItem>& items,
        const GachaPlayerState& state);

    std::shared_ptr<GachaRepository> gacha_;
    std::shared_ptr<PlayerRepository> players_;
    std::shared_ptr<RewardService> rewards_;
    std::shared_ptr<GachaTransactionRepository> gacha_transactions_;
    std::mt19937_64 random_;
};

}  // namespace mf::game 命名空间
