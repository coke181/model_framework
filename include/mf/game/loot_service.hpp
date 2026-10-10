#pragma once

// 文件职责：
// 封装掉落拾取业务，避免 UE 直接调用背包设置接口改资产。
// 调用流程：
// UE 请求 /loot/pickup -> LootService::pickup ->
// LootRepository 查掉落和标记已拾取 -> RewardService 发奖 -> 返回最新玩家资产。

#include "mf/game/mail_service.hpp"
#include "mf/game/models.hpp"
#include "mf/game/repositories.hpp"

#include <memory>
#include <string>

namespace mf::game {

class LootService {
public:
    LootService(
        std::shared_ptr<LootRepository> loot,
        std::shared_ptr<PlayerRepository> players,
        std::shared_ptr<RewardService> rewards,
        std::shared_ptr<LootTransactionRepository> loot_transactions = {});

    // spawn_for_player 生成一个归属某玩家的掉落。
    // 当前主要给后台/联调用；真正战斗服后续也应该调用这个语义，而不是直接发奖。
    ServiceResult<LootDrop> spawn_for_player(
        std::string owner_player_id,
        std::string scene_id,
        std::vector<RewardEntry> rewards,
        std::chrono::seconds ttl);

    // pickup 是拾取唯一入口。
    // 它保证同一个 drop_id 只能成功领取一次，并且最终通过 RewardService 入账。
    ServiceResult<LootPickupResult> pickup(const std::string& player_id, const std::string& drop_id);

private:
    std::shared_ptr<LootRepository> loot_;
    std::shared_ptr<PlayerRepository> players_;
    std::shared_ptr<RewardService> rewards_;
    std::shared_ptr<LootTransactionRepository> loot_transactions_;
};

}  // namespace mf::game 命名空间
