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
// 1. 承接抽卡玩家接口和后台卡池配置接口。
// 2. 玩家接口只暴露查询卡池、抽卡、看历史。
// 3. 后台接口负责配置卡池和奖品，并写审计日志。
//
// 文件调用流程：
// 1. main.cpp 构造 GachaService。
// 2. main.cpp 调用 register_gacha_routes(...)。
// 3. 玩家请求先通过 session 鉴权，再进入 GachaService。
// 4. 后台请求先通过 require_admin_permission 校验 admin.gacha_config。
// 5. 最终统一返回 JSON 给 UE 或后台页面。

namespace {

std::optional<mf::game::RewardKind> parse_gacha_reward_kind(std::string_view value) {
    if (value == "gold") {
        return mf::game::RewardKind::gold;
    }
    if (value == "currency") {
        return mf::game::RewardKind::currency;
    }
    if (value == "item") {
        return mf::game::RewardKind::item;
    }
    if (value == "attachment_template") {
        return mf::game::RewardKind::attachment_template;
    }
    return std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> read_optional_unix_time(
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

mf::game::RewardEntry read_reward_object(const boost::json::object& object) {
    const auto kind_text = read_string_field(object, "reward_kind");
    const auto kind = parse_gacha_reward_kind(kind_text);
    if (!kind) {
        throw mf::core::ApiException(mf::core::ApiError{
            .status = boost::beast::http::status::bad_request,
            .code = "invalid_reward_kind",
            .message = "reward_kind 只能是 gold、currency、item 或 attachment_template",
        });
    }
    return mf::game::RewardEntry{
        .kind = *kind,
        .resource_id = read_optional_string_field(object, "reward_resource_id"),
        .amount = read_optional_i64_field(object, "reward_amount", 1),
    };
}

}  // namespace

// 注册抽卡路由。
// 调用流程：
// 1. 玩家查询开放卡池 -> /gacha/pools。
// 2. 玩家抽卡 -> /gacha/draw -> GachaService::draw。
// 3. 玩家查历史 -> /gacha/history。
// 4. 管理员配置卡池/奖品 -> /admin/gacha/*。
void register_gacha_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::GachaService>& gacha_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/gacha/pools",
        [repositories](const mf::http::Request& request) {
            const auto pools = repositories.gacha->list_pools(false, std::chrono::system_clock::now());
            return mf::core::make_ok_response(
                boost::json::object{{"items", mf::game::to_json_array(pools)}},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/gacha/draw",
        [gacha_service, token_service](const mf::http::Request& request) {
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
            const auto pool_id = read_string_field(object, "pool_id");
            const auto count = read_optional_i64_field(object, "count", 1);
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            mf::core::add_current_span_attribute("gacha.pool_id", pool_id);
            mf::core::add_current_span_attribute("gacha.count", std::to_string(count));
            const auto result = gacha_service->draw(session->player_id, pool_id, count);
            if (result.error) {
                return mf::core::make_error_response(*result.error, request.version());
            }
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/gacha/history",
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
            const auto pool_id = read_optional_string_field(object, "pool_id");
            std::optional<mf::game::GachaHistoryCursor> gacha_cursor;
            const auto last_history_id = read_optional_string_field(object, "last_history_id");
            if (!last_history_id.empty()) {
                gacha_cursor = mf::game::GachaHistoryCursor{
                    .last_drawn_at = read_optional_i64_field(object, "last_drawn_at", 0),
                    .last_history_id = last_history_id,
                };
            }
            const auto page = repositories.gacha->list_history(session->player_id, pool_id, limit, gacha_cursor);
            return mf::core::make_ok_response(
                boost::json::object{
                    {"items", mf::game::to_json_array(page.items)},
                    {"limit", static_cast<std::int64_t>(limit)},
                    {"has_more", page.has_more},
                    {"next_drawn_at", page.next_drawn_at},
                    {"next_history_id", page.next_history_id},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/gacha/pools/upsert",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.gacha_config",
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
            const auto pool_id = read_string_field(object, "pool_id");
            const auto name = read_string_field(object, "name");
            if (pool_id.empty() || name.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "gacha_pool_required_fields_missing",
                        .message = "pool_id 和 name 不能为空",
                    },
                    request.version());
            }

            const auto saved = repositories.gacha->upsert_pool(mf::game::GachaPool{
                .pool_id = pool_id,
                .pool_type = read_optional_string_field(object, "pool_type").empty()
                                 ? std::string{"standard"}
                                 : read_optional_string_field(object, "pool_type"),
                .name = name,
                .description = read_optional_string_field(object, "description"),
                .banner_icon = read_optional_string_field(object, "banner_icon"),
                .cost_currency_id = read_optional_string_field(object, "cost_currency_id").empty()
                                        ? std::string{"gem"}
                                        : read_optional_string_field(object, "cost_currency_id"),
                .cost_amount_per_draw = std::max<std::int64_t>(
                    0,
                    read_optional_i64_field(object, "cost_amount_per_draw", 160)),
                .pity_5 = std::max<std::int64_t>(1, read_optional_i64_field(object, "pity_5", 90)),
                .pity_4 = std::max<std::int64_t>(1, read_optional_i64_field(object, "pity_4", 10)),
                .enabled = read_optional_bool_field(object, "enabled", true),
                .starts_at = read_optional_unix_time(object, "starts_at"),
                .ends_at = read_optional_unix_time(object, "ends_at"),
            });
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "gacha_pool_upsert",
                "gacha_pool",
                pool_id,
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(saved), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/gacha/items/upsert",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.gacha_config",
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
            const auto pool_id = read_string_field(object, "pool_id");
            const auto item_key = read_string_field(object, "item_key");
            if (pool_id.empty() || item_key.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "gacha_item_required_fields_missing",
                        .message = "pool_id 和 item_key 不能为空",
                    },
                    request.version());
            }
            if (!repositories.gacha->find_pool(pool_id)) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "gacha_pool_not_found",
                        .message = "请先创建卡池，再配置奖品",
                    },
                    request.version());
            }
            const auto reward = read_reward_object(object);
            if (reward.amount <= 0) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "invalid_reward_amount",
                        .message = "reward_amount 必须大于 0",
                    },
                    request.version());
            }
            const auto saved = repositories.gacha->upsert_pool_item(mf::game::GachaPoolItem{
                .pool_id = pool_id,
                .item_key = item_key,
                .rarity = std::max<std::int64_t>(1, read_optional_i64_field(object, "rarity", 3)),
                .weight = std::max<std::int64_t>(1, read_optional_i64_field(object, "weight", 1)),
                .is_up = read_optional_bool_field(object, "is_up", false),
                .reward = reward,
            });
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "gacha_item_upsert",
                "gacha_item",
                pool_id + ":" + item_key,
                boost::json::serialize(object));
            return mf::core::make_ok_response(mf::game::to_json(saved), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/gacha/pools/delete",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.gacha_config",
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
            const auto pool_id = read_string_field(object, "pool_id");
            if (pool_id.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "gacha_pool_id_required",
                        .message = "pool_id 不能为空",
                    },
                    request.version());
            }

            const auto before = repositories.gacha->find_pool(pool_id);
            const auto deleted = repositories.gacha->delete_pool(pool_id);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "gacha_pool_delete",
                "gacha_pool",
                pool_id,
                boost::json::serialize(object),
                deleted,
                deleted ? std::string{} : std::string{"卡池不存在"},
                before ? boost::json::serialize(mf::game::to_json(*before)) : std::string{},
                {});
            if (!deleted) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "gacha_pool_not_found",
                        .message = "卡池不存在，无法删除",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(
                boost::json::object{{"deleted", true}, {"pool_id", pool_id}},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/gacha/items/delete",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.gacha_config",
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
            const auto pool_id = read_string_field(object, "pool_id");
            const auto item_key = read_string_field(object, "item_key");
            if (pool_id.empty() || item_key.empty()) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::bad_request,
                        .code = "gacha_item_required_fields_missing",
                        .message = "pool_id 和 item_key 不能为空",
                    },
                    request.version());
            }

            const auto deleted = repositories.gacha->delete_pool_item(pool_id, item_key);
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "gacha_item_delete",
                "gacha_item",
                pool_id + ":" + item_key,
                boost::json::serialize(object),
                deleted,
                deleted ? std::string{} : std::string{"奖品不存在"});
            if (!deleted) {
                return mf::core::make_error_response(
                    {
                        .status = boost::beast::http::status::not_found,
                        .code = "gacha_item_not_found",
                        .message = "卡池奖品不存在，无法删除",
                    },
                    request.version());
            }
            return mf::core::make_ok_response(
                boost::json::object{{"deleted", true}, {"pool_id", pool_id}, {"item_key", item_key}},
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/gacha/query",
        [repositories, token_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.gacha_config",
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
            const auto pool_id = read_optional_string_field(object, "pool_id");
            const auto pools = repositories.gacha->list_pools(include_disabled, std::chrono::system_clock::now());
            boost::json::array pool_items;
            for (const auto& pool : pools) {
                if (!pool_id.empty() && pool.pool_id != pool_id) {
                    continue;
                }
                pool_items.push_back(boost::json::object{
                    {"pool", mf::game::to_json(pool)},
                    {"items", mf::game::to_json_array(repositories.gacha->list_pool_items(pool.pool_id))},
                });
            }
            return mf::core::make_ok_response(
                boost::json::object{{"items", std::move(pool_items)}},
                request.version());
        });
}
