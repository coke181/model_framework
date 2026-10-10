#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"
#include "mf/game/json.hpp"

#include <boost/json/serialize.hpp>

#include <algorithm>

// 文件职责：
// 1. 提供 UE 任务定义、玩家任务快照、接受、事件推进和领奖接口。
// 2. 提供管理员任务定义写入接口，便于先配置任务再让 UE 拉取。
// 3. 普通客户端只提交事件，不允许直接提交完成状态或奖励内容。
//
// 文件调用流程：
// main.cpp 构造 QuestService -> register_quest_routes 注册路由 ->
// 路由校验 access token/管理员权限并解析 JSON -> QuestService -> QuestRepository 事务落库。

namespace {

mf::game::QuestClaimMode parse_claim_mode(const std::string& value) {
    if (value == "auto") return mf::game::QuestClaimMode::automatic;
    if (value == "npc") return mf::game::QuestClaimMode::npc;
    return mf::game::QuestClaimMode::manual;
}

std::vector<mf::game::QuestObjectiveDefinition> read_objectives(const boost::json::object& object) {
    std::vector<mf::game::QuestObjectiveDefinition> objectives;
    const auto* value = object.if_contains("objectives");
    if (!value || !value->is_array()) return objectives;
    for (const auto& item : value->as_array()) {
        if (!item.is_object()) continue;
        const auto& objective = item.as_object();
        objectives.push_back({
            .objective_id = read_string_field(objective, "objective_id"),
            .event_type = read_string_field(objective, "event_type"),
            .target_id = read_optional_string_field(objective, "target_id"),
            .target_value = read_optional_i64_field(objective, "target_value", 1),
        });
    }
    return objectives;
}

}  // namespace

void register_quest_routes(
    mf::core::Application& app,
    const mf::game::RepositoryBundle& repositories,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::game::QuestService>& quest_service) {
    app.route(
        mf::http::beast_http::verb::get,
        "/quests",
        [token_service, quest_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            if (!require_session(request, *token_service, storage, error)) {
                return mf::core::make_error_response(error, request.version());
            }
            const auto definitions = quest_service->list_definitions(false);
            std::int64_t definition_version = 0;
            for (const auto& definition : definitions) {
                definition_version = std::max(definition_version, definition.version);
            }
            return mf::core::make_ok_response(
                boost::json::object{
                    {"definition_version", definition_version},
                    {"changed", true},
                    {"quests", mf::game::to_json_array(definitions)},
                },
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::get,
        "/players/me/quests",
        [token_service, quest_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            return mf::core::make_ok_response(
                mf::game::to_json(quest_service->get_snapshot(session->player_id)),
                request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/quests/accept",
        [token_service, quest_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto result = quest_service->accept(
                session->player_id,
                read_string_field(object, "quest_id"),
                read_string_field(object, "request_id"));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    const auto event_handler = [token_service, quest_service](const mf::http::Request& request) {
        mf::core::ApiError error;
        mf::game::Session storage;
        const auto* session = require_session(request, *token_service, storage, error);
        if (!session) return mf::core::make_error_response(error, request.version());
        const auto body = mf::core::parse_json_body(request, error);
        if (!body) return mf::core::make_error_response(error, request.version());
        const auto& object = body->as_object();
        const auto result = quest_service->apply_event(
            session->player_id,
            read_string_field(object, "event_id"),
            read_string_field(object, "event_type"),
            read_optional_string_field(object, "target_id"),
            read_optional_i64_field(object, "amount", 1),
            read_optional_string_field(object, "source_id"));
        if (result.error) return mf::core::make_error_response(*result.error, request.version());
        return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
    };
    app.route(mf::http::beast_http::verb::post, "/quests/events", event_handler);
    app.route(mf::http::beast_http::verb::post, "/quests/progress", event_handler);

    app.route(
        mf::http::beast_http::verb::post,
        "/quests/claim",
        [token_service, quest_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto result = quest_service->claim(
                session->player_id,
                read_string_field(object, "quest_id"),
                read_string_field(object, "request_id"),
                read_optional_string_field(object, "npc_id"));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });

    app.route(
        mf::http::beast_http::verb::post,
        "/admin/quests/upsert",
        [repositories, token_service, quest_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            mf::game::AdminPrincipal admin_storage;
            const auto* admin = require_admin_permission(
                request,
                *token_service,
                repositories.admins,
                "admin.quest_config",
                session_storage,
                admin_storage,
                error);
            if (!admin) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            std::string reward_error;
            const auto rewards = read_rewards_array(object, reward_error);
            if (!rewards) {
                return mf::core::make_error_response(
                    {.status = boost::beast::http::status::bad_request,
                     .code = "invalid_quest_rewards",
                     .message = reward_error.empty() ? "任务奖励配置不合法" : reward_error},
                    request.version());
            }
            const auto result = quest_service->upsert_definition(mf::game::QuestDefinition{
                .quest_id = read_string_field(object, "quest_id"),
                .version = read_optional_i64_field(object, "version", 1),
                .title = read_string_field(object, "title"),
                .description = read_optional_string_field(object, "description"),
                .claim_mode = parse_claim_mode(read_optional_string_field(object, "claim_mode")),
                .turn_in_npc_id = read_optional_string_field(object, "turn_in_npc_id"),
                .completion_flag = read_optional_string_field(object, "completion_flag"),
                .pre_quest_ids = read_string_array_field(object, "pre_quest_ids"),
                .required_flags = read_string_array_field(object, "required_flags"),
                .objectives = read_objectives(object),
                .rewards = *rewards,
                .enabled = read_optional_bool_field(object, "enabled", true),
            });
            append_admin_audit(
                repositories.audit_logs,
                request,
                admin->operator_name,
                "quest_upsert",
                "quest",
                read_string_field(object, "quest_id"),
                boost::json::serialize(object),
                !result.error,
                result.error ? result.error->message : std::string{});
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::game::to_json(*result.value), request.version());
        });
}
