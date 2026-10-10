#include "hello_route_support.hpp"
#include "hello_routes.hpp"

#include "mf/core/api.hpp"
#include "mf/core/application.hpp"
#include "mf/core/tracing.hpp"

#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>

// 文件职责：提供 Agent 聊天、确认、历史和会话删除接口。
// 调用流程：游戏 Bearer Token -> AgentService -> 权威上下文/模型 Provider/AgentRepository。

namespace {

std::int64_t unix_seconds(const std::optional<std::chrono::system_clock::time_point>& value) {
    if (!value) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(value->time_since_epoch()).count();
}

boost::json::value parsed_json_or_empty(const std::string& value) {
    try { return boost::json::parse(value); } catch (...) { return boost::json::object{}; }
}

boost::json::object tool_call_json(const mf::agent::AgentToolCall& call) {
    return {
        {"call_id", call.call_id},
        {"tool_name", call.tool_name},
        {"status", call.status},
        {"risk", call.risk_level},
        {"display_text", call.display_text},
        {"arguments", parsed_json_or_empty(call.arguments_json)},
        {"error_code", call.error_code},
        {"created_at", unix_seconds(call.created_at)},
        {"confirmed_at", unix_seconds(call.confirmed_at)},
        {"finished_at", unix_seconds(call.finished_at)},
    };
}

boost::json::object message_json(const mf::agent::AgentMessage& message) {
    return {
        {"message_id", message.message_id},
        {"conversation_id", message.conversation_id},
        {"role", message.role},
        {"content", message.content},
        {"model", message.model},
        {"created_at", unix_seconds(message.created_at)},
    };
}

boost::json::object chat_output_json(const mf::agent::AgentChatOutput& output) {
    boost::json::array calls;
    for (const auto& call : output.tool_calls) calls.push_back(tool_call_json(call));
    boost::json::array actions;
    for (const auto& action : output.suggested_actions) actions.push_back(action);
    return {
        {"conversation_id", output.conversation_id},
        {"message_id", output.message_id},
        {"reply", output.reply},
        {"used_fallback", output.used_fallback},
        {"tool_calls", std::move(calls)},
        {"suggested_actions", std::move(actions)},
    };
}

}  // namespace

void register_agent_routes(
    mf::core::Application& app,
    const std::shared_ptr<mf::auth::TokenService>& token_service,
    const std::shared_ptr<mf::agent::AgentService>& agent_service,
    const std::shared_ptr<mf::agent::AgentTaskService>& agent_task_service) {
    app.route(mf::http::beast_http::verb::post, "/api/agent/chat",
        [token_service, agent_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            std::string context_json;
            if (const auto* context = object.if_contains("context")) context_json = boost::json::serialize(*context);
            mf::core::add_current_span_attribute("game.player_id", session->player_id);
            const auto result = agent_service->chat(session->player_id, session->nickname, {
                .conversation_id = read_optional_string_field(object, "conversation_id"),
                .client_request_id = read_string_field(object, "client_request_id"),
                .message = read_string_field(object, "message"),
                .scene = read_optional_string_field(object, "scene"),
                .client_context_json = std::move(context_json),
            });
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            mf::core::add_current_span_attribute("agent.conversation_id", result.value->conversation_id);
            return mf::core::make_ok_response(chat_output_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/confirm",
        [token_service, agent_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto result = agent_service->confirm(session->player_id, {
                .conversation_id = read_string_field(object, "conversation_id"),
                .call_id = read_string_field(object, "call_id"),
                .client_request_id = read_string_field(object, "client_request_id"),
                .approved = read_optional_bool_field(object, "approved", false),
            });
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            boost::json::object response{
                {"call_id", result.value->tool_call.call_id},
                {"status", result.value->tool_call.status},
                {"result_text", result.value->result_text},
            };
            response["client_action"] = result.value->client_action
                ? boost::json::value{*result.value->client_action}
                : boost::json::value{nullptr};
            return mf::core::make_ok_response(response, request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/history",
        [token_service, agent_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(
                read_optional_i64_field(object, "limit", 30), 1, 100));
            const auto result = agent_service->history(
                session->player_id, read_string_field(object, "conversation_id"), limit);
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            boost::json::array messages;
            for (const auto& message : *result.value) messages.push_back(message_json(message));
            return mf::core::make_ok_response(boost::json::object{{"messages", std::move(messages)}}, request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/conversations/delete",
        [token_service, agent_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session session_storage;
            const auto* session = require_session(request, *token_service, session_storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto result = agent_service->delete_conversation(
                session->player_id, read_string_field(body->as_object(), "conversation_id"));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(boost::json::object{{"deleted", *result.value}}, request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/tasks/create",
        [token_service, agent_task_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            std::vector<mf::agent::AgentTaskStepState> steps;
            if (const auto* raw = object.if_contains("steps"); raw && raw->is_array()) {
                for (const auto& item : raw->as_array()) {
                    if (!item.is_object()) continue;
                    const auto& step = item.as_object();
                    steps.push_back({
                        .step_id = read_string_field(step, "step_id"),
                        .tool_name = read_string_field(step, "tool"),
                        .title = read_optional_string_field(step, "title"),
                    });
                }
            }
            const auto result = agent_task_service->create(
                session->player_id, read_string_field(object, "goal"), std::move(steps));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::agent::to_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/tasks/get",
        [token_service, agent_task_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto result = agent_task_service->get(
                session->player_id, read_string_field(body->as_object(), "task_id"));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::agent::to_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/tasks/step-result",
        [token_service, agent_task_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto& object = body->as_object();
            const auto result = agent_task_service->record_step_result(
                session->player_id, read_string_field(object, "task_id"),
                read_string_field(object, "step_id"), read_optional_bool_field(object, "success", false),
                object.if_contains("result") ? *object.if_contains("result") : boost::json::value(nullptr),
                read_optional_string_field(object, "error"));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::agent::to_json(*result.value), request.version());
        });

    app.route(mf::http::beast_http::verb::post, "/api/agent/tasks/cancel",
        [token_service, agent_task_service](const mf::http::Request& request) {
            mf::core::ApiError error;
            mf::game::Session storage;
            const auto* session = require_session(request, *token_service, storage, error);
            if (!session) return mf::core::make_error_response(error, request.version());
            const auto body = mf::core::parse_json_body(request, error);
            if (!body) return mf::core::make_error_response(error, request.version());
            const auto result = agent_task_service->cancel(
                session->player_id, read_string_field(body->as_object(), "task_id"));
            if (result.error) return mf::core::make_error_response(*result.error, request.version());
            return mf::core::make_ok_response(mf::agent::to_json(*result.value), request.version());
        });
}
