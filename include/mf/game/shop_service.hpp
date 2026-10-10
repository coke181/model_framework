#pragma once

// 文件职责：
// 封装商城购买业务，避免 HTTP handler 直接操作商品表、玩家资产和奖励流水。
// 调用流程：
// UE 请求 /shop/buy -> ShopService::purchase ->
// ShopRepository 查商品/限购 -> PlayerRepository 扣货币 ->
// RewardService 发奖 -> ShopRepository 写购买流水。

#include "mf/game/mail_service.hpp"
#include "mf/game/models.hpp"
#include "mf/game/repositories.hpp"

#include <memory>
#include <string>

namespace mf::game {

class ShopService {
public:
    ShopService(
        std::shared_ptr<ShopRepository> shop,
        std::shared_ptr<PlayerRepository> players,
        std::shared_ptr<RewardService> rewards,
        std::shared_ptr<ShopTransactionRepository> shop_transactions = {});

    // list_products 返回当前可展示商品。
    // include_disabled 只应给后台使用；玩家侧接口永远只拿开放且在时间窗口内的商品。
    std::vector<ShopProduct> list_products(bool include_disabled) const;

    // purchase 是商城购买唯一入口。
    // 它负责校验商品、限购、扣费、发奖和写流水，UE 不能绕过它直接改背包。
    ServiceResult<ShopPurchaseResult> purchase(const std::string& player_id, const std::string& product_id);

private:
    // 限购商品的原子购买路径：占名额、扣费、发奖、写流水在同一个存储事务内完成。
    ServiceResult<ShopPurchaseResult> purchase_with_transaction(
        const std::string& player_id,
        const ShopProduct& product);

    std::shared_ptr<ShopRepository> shop_;
    std::shared_ptr<PlayerRepository> players_;
    std::shared_ptr<RewardService> rewards_;
    std::shared_ptr<ShopTransactionRepository> shop_transactions_;
};

}  // namespace mf::game 命名空间
