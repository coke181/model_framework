#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>

// 文件职责：
// 1. 承接玩家商城接口和后台商品配置接口。
// 2. 玩家只能查开放商品、购买商品、查自己的购买历史。
// 3. 后台通过 admin.shop_config 权限维护商品配置。
//
// 文件调用流程：
// 1. main.cpp 构造 ShopService。
// 2. main.cpp 调用 register_shop_routes(...)。
// 3. 玩家请求先通过 session 鉴权，再进入 ShopService。
// 4. 后台请求先通过 require_admin_permission 校验 admin.shop_config。
// 5. 最终统一返回 JSON 给 UE 或后台页面。

namespace {

std::optional<std::chrono::system_clock::time_point> read_optional_unix_time_local(
    const boost::json::object& object,
    std::string_view key) {
    if (!object.if_contains(key)) {
        return std::nullopt;
    }
    const auto value = read_optional_i64_field(object, key, 0);
    if (value <= 0) {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point{std::chrono::seconds{value}};
}

mf::game::ShopProduct read_shop_product_from_body(const boost::json::object& object) {
    std::string reward_error;
    const auto rewards = read_rewards_array(object, reward_error);
    if (!rewards) {
        throw mf::core::ApiException(mf::core::ApiError{
            .status = boost::beast::http::status::bad_request,
            .code = "invalid_shop_rewards",
            .message = reward_error.empty() ? "商品奖励配置不合法" : reward_error,
        });
    }

    auto category = read_optional_string_field(object, "category");
    auto price_currency_id = read_optional_string_field(object, "price_currency_id");
    if (category.empty()) {
        category = "default";
    }
    if (price_currency_id.empty()) {
        price_currency_id = "gem";
    }

    return mf::game::ShopProduct{
        .product_id = read_string_field(object, "product_id"),
        .category = std::move(category),
        .name = read_string_field(object, "name"),
        .description = read_optional_string_field(object, "description"),
        .icon = read_optional_string_field(object, "icon"),
        .price_currency_id = std::move(price_currency_id),
        .price_amount = std::max<std::int64_t>(0, read_optional_i64_field(object, "price_amount", 0)),
        .rewards = *rewards,
        .purchase_limit = std::max<std::int64_t>(0, read_optional_i64_field(object, "purchase_limit", 0)),
        .enabled = read_optional_bool_field(object, "enabled", true),
        .starts_at = read_optional_unix_time_local(object, "starts_at"),
        .ends_at = read_optional_unix_time_local(object, "ends_at"),
    };
}

}  // namespace

// 注册商城路由。
// 玩家侧接口给 UE 使用；后台接口给 GM 页面或运营工具使用。
void register_shop_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::ShopService>& shop_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/shop/products",
        [shop_service](const mf::http::Request& request) {
            const auto products = shop_service->list_products(false);
            return mf::core::make_ok_response(
                boost::json::object{{"items", mf::game::to_json_array(products)}},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/shop/buy",
        [shop_service, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto& object = body->as_object();
            const auto product_id = read_string_field(object, "product_id");
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("shop.product_id", product_id);
            const auto result = shop_service->purchase(session->player_id, product_id);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/shop/history",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto& object = body->as_object();
            const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(
                1,
                std::min<std::int64_t>(100, read_optional_i64_field(object, "limit", 20))));
            std::optional<mf::game::ShopPurchaseCursor> purchase_cursor;
            const auto last_purchase_id = read_optional_string_field(object, "last_purchase_id");
            if (!last_purchase_id.empty()) {
                purchase_cursor = mf::game::ShopPurchaseCursor{
                    .last_purchased_at = read_optional_i64_field(object, "last_purchased_at", 0),
                    .last_purchase_id = last_purchase_id,
                };
            }
            const auto page = repositories.shop->list_purchases(session->player_id, limit, purchase_cursor);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"items", mf::game::to_json_array(page.items)},
                    {"limit", static_cast<std::int64_t>(limit)},
                    {"has_more", page.has_more},
                    {"next_purchased_at", page.next_purchased_at},
                    {"next_purchase_id", page.next_purchase_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/shop/products/upsert",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.shop_config",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto& object = body->as_object();
            auto product = read_shop_product_from_body(object);
            if (product.product_id.empty() || product.name.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "shop_product_required_fields_missing",
                        .message = "product_id 和 name 不能为空",
                    },
                    request.version());
            }
            if (product.rewards.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "shop_product_rewards_empty",
                        .message = "商品至少需要配置一个奖励",
                    },
                    request.version());
            }

            const auto before = repositories.shop->find_product(product.product_id);
            const auto product_id = product.product_id;
            const auto saved = repositories.shop->upsert_product(std::move(product));
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "shop_product_upsert",
                "shop_product",
                product_id,
                boost::json::serialize(object),
                true,
                {},
                before ? boost::json::serialize(mf::game::to_json(*before)) : std::string{},
                boost::json::serialize(mf::game::to_json(saved)));
            return mf::core::make_ok_response(mf::game::to_json(saved), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/shop/products/delete",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.shop_config",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto& object = body->as_object();
            const auto product_id = read_string_field(object, "product_id");
            if (product_id.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "shop_product_id_required",
                        .message = "product_id 不能为空",
                    },
                    request.version());
            }
            const auto before = repositories.shop->find_product(product_id);
            const auto deleted = repositories.shop->delete_product(product_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "shop_product_delete",
                "shop_product",
                product_id,
                boost::json::serialize(object),
                deleted,
                deleted ? std::string{} : std::string{"商品不存在"},
                before ? boost::json::serialize(mf::game::to_json(*before)) : std::string{},
                {});
            if (!deleted) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "shop_product_not_found",
                        .message = "商品不存在，无法删除",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(
                boost::json::object{{"deleted", true}, {"product_id", product_id}},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/shop/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.shop_config",
                session_storage,
                admin_storage,
                error);
            if (!admin) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto& object = body->as_object();
            const auto include_disabled = read_optional_bool_field(object, "include_disabled", true);
            const auto player_id = read_optional_string_field(object, "player_id");
            const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(
                1,
                std::min<std::int64_t>(100, read_optional_i64_field(object, "limit", 20))));
            std::optional<mf::game::ShopPurchaseCursor> purchase_cursor;
            const auto last_purchase_id = read_optional_string_field(object, "last_purchase_id");
            if (!last_purchase_id.empty()) {
                purchase_cursor = mf::game::ShopPurchaseCursor{
                    .last_purchased_at = read_optional_i64_field(object, "last_purchased_at", 0),
                    .last_purchase_id = last_purchase_id,
                };
            }
            boost::json::object response{
                {"products", mf::game::to_json_array(repositories.shop->list_products(include_disabled, std::chrono::system_clock::now()))},
            };
            if (!player_id.empty()) {
                const auto page = repositories.shop->list_purchases(player_id, limit, purchase_cursor);
                response["purchases"] = mf::game::to_json_array(page.items);
                response["limit"] = static_cast<std::int64_t>(limit);
                response["has_more"] = page.has_more;
                response["next_purchased_at"] = page.next_purchased_at;
                response["next_purchase_id"] = page.next_purchase_id;
            }
            return mf::core::make_ok_response(std::move(response), request.version());
        });
}
