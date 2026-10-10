#include "mf/game/shop_service.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现商城购买闭环。
// 调用流程：
// /shop/buy -> ShopService::purchase -> 商品校验 -> 扣货币 -> 发奖励 -> 记购买流水。

#include <boost/beast/http/status.hpp>

#include <chrono>
#include <sstream>
#include <utility>

namespace mf::game {
namespace {

// 生成购买流水 ID。
// 这里使用玩家、商品和微秒时间组合，足够支撑当前单机服务；以后可替换成雪花 ID 或数据库序列。
std::string make_purchase_id(const std::string& player_id, const std::string& product_id) {
    const auto tick = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::ostringstream output;
    output << "shop_" << player_id << '_' << product_id << '_' << tick;
    return output.str();
}

// 判断商品是否处于玩家可购买状态。
// starts_at / ends_at 为空表示不限制对应边界。
bool product_is_open(const ShopProduct& product, std::chrono::system_clock::time_point now) {
    if (!product.enabled) {
        return false;
    }
    if (product.starts_at && now < *product.starts_at) {
        return false;
    }
    if (product.ends_at && now > *product.ends_at) {
        return false;
    }
    return true;
}

}  // 匿名命名空间

// 构造商城服务，注入商品仓储、玩家仓储和奖励服务。
ShopService::ShopService(
    std::shared_ptr<ShopRepository> shop,
    std::shared_ptr<PlayerRepository> players,
    std::shared_ptr<RewardService> rewards,
    std::shared_ptr<ShopTransactionRepository> shop_transactions)
    : shop_(std::move(shop)),
      players_(std::move(players)),
      rewards_(std::move(rewards)),
      shop_transactions_(std::move(shop_transactions)) {}

// 返回商城商品列表。
// 玩家侧不传 include_disabled，所以只会看到启用且时间窗口开放的商品。
std::vector<ShopProduct> ShopService::list_products(bool include_disabled) const {
    MF_PROFILE_SCOPE("Shop list products");
    return shop_->list_products(include_disabled, std::chrono::system_clock::now());
}

// 执行一次购买。
// 当前版本扣费、发奖、写购买流水分三步完成；如果后续要求更强一致性，应在 MySQL 仓储里升级为事务或补偿任务。
ServiceResult<ShopPurchaseResult> ShopService::purchase(
    const std::string& player_id,
    const std::string& product_id) {
    if (product_id.empty()) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "shop_product_id_required",
                .message = "product_id 不能为空",
            },
        };
    }

    const auto product = shop_->find_product(product_id);
    if (!product) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "shop_product_not_found",
                .message = "商品不存在",
            },
        };
    }

    if (!product_is_open(*product, std::chrono::system_clock::now())) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::forbidden,
                .code = "shop_product_closed",
                .message = "商品未开放或已下架",
            },
        };
    }

    if (product->price_amount < 0 || product->price_currency_id.empty()) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "shop_product_price_invalid",
                .message = "商品价格配置不合法",
            },
        };
    }
    if (product->rewards.empty()) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "shop_product_rewards_empty",
                .message = "商品没有配置任何奖励，无法购买",
            },
        };
    }

    if (product->purchase_limit > 0 && shop_transactions_) {
        return purchase_with_transaction(player_id, *product);
    }

    if (product->purchase_limit > 0) {
        const auto purchased_count = shop_->count_purchases(player_id, product_id);
        if (purchased_count >= product->purchase_limit) {
            return ServiceResult<ShopPurchaseResult>{
                .error = mf::core::ApiError{
                    .status = boost::beast::http::status::forbidden,
                    .code = "shop_purchase_limit_reached",
                    .message = "该商品已达到购买次数限制",
                    .details = {
                        {"product_id", product_id},
                        {"purchase_limit", product->purchase_limit},
                        {"purchased_count", purchased_count},
                    },
                },
            };
        }
    }

    std::optional<PlayerProfile> player_after_spend;
    if (product->price_amount > 0) {
        player_after_spend = players_->spend_currency(player_id, product->price_currency_id, product->price_amount);
        if (!player_after_spend) {
            return ServiceResult<ShopPurchaseResult>{
                .error = mf::core::ApiError{
                    .status = boost::beast::http::status::bad_request,
                    .code = "currency_not_enough",
                    .message = "货币不足，无法购买商品",
                    .details = {
                        {"currency_id", product->price_currency_id},
                        {"required", product->price_amount},
                    },
                },
            };
        }
    } else {
        player_after_spend = players_->find_by_player_id(player_id);
    }

    if (!player_after_spend) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "player_not_found",
                .message = "玩家不存在，无法购买商品",
            },
        };
    }

    const auto purchase_id = make_purchase_id(player_id, product_id);
    const auto grant = rewards_->grant_to_player(
        player_id,
        purchase_id,
        "shop",
        product_id,
        product->rewards);
    if (grant.error) {
        return ServiceResult<ShopPurchaseResult>{.error = grant.error};
    }

    auto purchase = shop_->append_purchase(ShopPurchaseRecord{
        .purchase_id = purchase_id,
        .player_id = player_id,
        .product_id = product_id,
        .price_currency_id = product->price_currency_id,
        .price_amount = product->price_amount,
        .rewards = product->rewards,
        .purchased_at = std::chrono::system_clock::now(),
    });

    const auto player_after_grant = players_->find_by_player_id(player_id).value_or(*player_after_spend);
    return ServiceResult<ShopPurchaseResult>{
        .value = ShopPurchaseResult{
            .product = *product,
            .purchase = std::move(purchase),
            .grant = grant.value,
            .player = player_after_grant,
        },
    };
}

// 限购商品走原子事务购买，避免“先查再写”的软上限在并发下被突破。
ServiceResult<ShopPurchaseResult> ShopService::purchase_with_transaction(
    const std::string& player_id,
    const ShopProduct& product) {
    const auto purchase_id = make_purchase_id(player_id, product.product_id);
    const auto result = shop_transactions_->purchase_once(
        purchase_id,
        player_id,
        product,
        std::chrono::system_clock::now());

    if (result.limit_reached) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::forbidden,
                .code = "shop_purchase_limit_reached",
                .message = "该商品已达到购买次数限制",
                .details = {
                    {"product_id", product.product_id},
                    {"purchase_limit", product.purchase_limit},
                    {"purchased_count", result.purchased_count},
                },
            },
        };
    }
    if (result.currency_not_enough) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "currency_not_enough",
                .message = "货币不足，无法购买商品",
                .details = {
                    {"currency_id", product.price_currency_id},
                    {"required", product.price_amount},
                },
            },
        };
    }
    if (result.player_not_found) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::not_found,
                .code = "player_not_found",
                .message = "玩家不存在，无法购买商品",
            },
        };
    }
    if (!result.purchase || !result.player) {
        return ServiceResult<ShopPurchaseResult>{
            .error = mf::core::ApiError{
                .status = boost::beast::http::status::internal_server_error,
                .code = "shop_purchase_failed",
                .message = "购买失败，请稍后重试",
            },
        };
    }

    return ServiceResult<ShopPurchaseResult>{
        .value = ShopPurchaseResult{
            .product = product,
            .purchase = std::move(*result.purchase),
            .grant = result.grant,
            .player = std::move(*result.player),
        },
    };
}

}  // namespace mf::game 命名空间
