#include "mf/agent/service.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

// 文件职责：实现 Agent 会话、模型调用、降级、确认绑定、幂等和每日限流。

namespace mf::agent {
namespace {

mf::core::ApiError agent_error(
    boost::beast::http::status status,
    std::string code,
    std::string message) {
    return {.status = status, .code = std::move(code), .message = std::move(message)};
}

std::string random_id(std::string_view prefix) {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    std::ostringstream output;
    output << prefix << std::hex << std::setw(16) << std::setfill('0') << generator();
    return output.str();
}

bool valid_id(const std::string& value, std::size_t max_length) {
    return !value.empty() && value.size() <= max_length &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == ':';
           });
}

std::string utc_date() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value, &now);
#else
    gmtime_r(&now, &value);
#endif
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%d");
    return output.str();
}

std::string trim_to_bytes(std::string value, std::size_t limit) {
    if (value.size() > limit) value.resize(limit);
    return value;
}

}  // namespace

AgentService::AgentService(
    AgentOptions options,
    std::shared_ptr<AgentRepository> repository,
    std::shared_ptr<AgentModelProvider> model_provider,
    std::shared_ptr<AgentToolRegistry> tool_registry)
    : options_(std::move(options)),
      repository_(std::move(repository)),
      model_provider_(std::move(model_provider)),
      tool_registry_(std::move(tool_registry)) {}

// 处理一次玩家消息。模型只能生成文本，结构化工具由后端白名单独立产生并等待确认。
mf::game::ServiceResult<AgentChatOutput> AgentService::chat(
    const std::string& player_id,
    const std::string& display_name,
    AgentChatInput input) const {
    if (input.message.empty() || input.message.size() > options_.max_message_length ||
        !valid_id(input.client_request_id, 128)) {
        return {.error = agent_error(
            boost::beast::http::status::bad_request,
            "agent_invalid_request",
            "message 不能为空且不能超过限制，client_request_id 必须是合法且稳定的幂等键")};
    }
    if (input.scene.empty()) input.scene = "unknown";
    if (input.scene.size() > 32) {
        return {.error = agent_error(boost::beast::http::status::bad_request, "agent_invalid_request", "scene 长度不能超过 32")};
    }

    if (const auto existing = repository_->find_assistant_message_by_request(player_id, input.client_request_id)) {
        return {.value = AgentChatOutput{
            .conversation_id = existing->conversation_id,
            .message_id = existing->message_id,
            .reply = existing->content,
            .used_fallback = existing->model == "local_fallback",
            .tool_calls = repository_->list_tool_calls_for_message(existing->message_id),
        }};
    }

    const auto usage = repository_->get_daily_usage(player_id, utc_date());
    if (usage.request_count >= options_.daily_request_limit) {
        return {.error = agent_error(
            boost::beast::http::status::too_many_requests,
            "agent_rate_limited",
            "今日 Agent 请求次数已达到上限，请稍后再试")};
    }

    AgentConversation conversation;
    if (!input.conversation_id.empty()) {
        if (!valid_id(input.conversation_id, 96)) {
            return {.error = agent_error(boost::beast::http::status::bad_request, "agent_invalid_request", "conversation_id 格式不合法")};
        }
        if (const auto existing = repository_->find_conversation(input.conversation_id, player_id)) {
            conversation = *existing;
            if (conversation.status != "active") {
                return {.error = agent_error(boost::beast::http::status::not_found, "agent_conversation_not_found", "Agent 会话不存在或已删除")};
            }
        } else {
            return {.error = agent_error(
                boost::beast::http::status::not_found,
                "agent_conversation_not_found",
                "指定的 Agent 会话不存在；新会话请省略 conversation_id，由后端生成")};
        }
    } else {
        conversation = repository_->save_conversation({
            .conversation_id = random_id("agent_conv_"),
            .player_id = player_id,
            .scene = input.scene,
            .title = input.message.substr(0, 64),
        });
    }

    const auto history = repository_->list_messages(
        conversation.conversation_id, player_id, options_.max_history_messages);
    repository_->append_message({
        .message_id = random_id("agent_msg_"),
        .conversation_id = conversation.conversation_id,
        .player_id = player_id,
        .client_request_id = input.client_request_id,
        .role = "user",
        .content = input.message,
    });

    const auto authoritative_context = trim_to_bytes(
        tool_registry_->build_authoritative_context(player_id), options_.max_context_bytes);
    const auto client_context = trim_to_bytes(std::move(input.client_context_json), options_.max_context_bytes / 2);
    const auto model_response = model_provider_->complete({
        .system_prompt =
            "你是 Blaster 游戏内助手。只依据后端权威上下文回答账号、资产、任务、邮件和好友问题。"
            "客户端上下文只用于战术建议。不要声称已经执行操作；操作必须通过后端结构化工具确认。回答使用简洁中文。",
        .history = history,
        .user_message = input.message,
        .authoritative_context = authoritative_context,
        .client_context = client_context,
    });
    const auto used_fallback = !model_response.success;
    const auto reply = model_response.success
        ? model_response.content
        : fallback_reply(input.message, authoritative_context);

    auto assistant = repository_->append_message({
        .message_id = random_id("agent_msg_"),
        .conversation_id = conversation.conversation_id,
        .player_id = player_id,
        .client_request_id = input.client_request_id,
        .role = "assistant",
        .content = reply,
        .model = model_response.success ? options_.model : "local_fallback",
        .prompt_tokens = model_response.prompt_tokens,
        .completion_tokens = model_response.completion_tokens,
    });

    std::vector<AgentToolCall> tool_calls;
    if (auto suggested = tool_registry_->suggest_tool(
            player_id,
            conversation.conversation_id,
            assistant.message_id,
            input.client_request_id,
            input.message)) {
        tool_calls.push_back(repository_->save_tool_call(std::move(*suggested)));
    }
    repository_->record_usage(
        player_id,
        utc_date(),
        model_response.prompt_tokens,
        model_response.completion_tokens,
        !model_response.success);

    (void)display_name;
    return {.value = AgentChatOutput{
        .conversation_id = conversation.conversation_id,
        .message_id = assistant.message_id,
        .reply = reply,
        .used_fallback = used_fallback,
        .tool_calls = std::move(tool_calls),
    }};
}

// 确认请求严格绑定 player_id、conversation_id、call_id，并以工具状态保证重复确认不会重复执行。
mf::game::ServiceResult<AgentConfirmOutput> AgentService::confirm(
    const std::string& player_id,
    AgentConfirmInput input) const {
    if (!valid_id(input.conversation_id, 96) || !valid_id(input.call_id, 128) ||
        !valid_id(input.client_request_id, 128)) {
        return {.error = agent_error(boost::beast::http::status::bad_request, "agent_invalid_request", "确认请求字段格式不合法")};
    }
    auto tool_call = repository_->find_tool_call(input.call_id, input.conversation_id, player_id);
    if (!tool_call) {
        return {.error = agent_error(boost::beast::http::status::not_found, "agent_tool_call_not_found", "Agent 工具调用不存在或不属于当前玩家")};
    }
    if (tool_call->status == "succeeded" || tool_call->status == "rejected" || tool_call->status == "failed") {
        AgentConfirmOutput output{.tool_call = *tool_call};
        if (!tool_call->result_json.empty()) {
            try {
                const auto result = boost::json::parse(tool_call->result_json).as_object();
                if (const auto* text = result.if_contains("result_text"); text && text->is_string()) output.result_text = text->as_string().c_str();
                if (const auto* action = result.if_contains("client_action"); action && action->is_object()) output.client_action = action->as_object();
            } catch (...) {}
        }
        return {.value = std::move(output)};
    }
    const auto now = std::chrono::system_clock::now();
    if (!tool_call->created_at || now > *tool_call->created_at + options_.confirmation_ttl) {
        tool_call->status = "failed";
        tool_call->error_code = "agent_confirmation_expired";
        tool_call->finished_at = now;
        *tool_call = repository_->save_tool_call(std::move(*tool_call));
        return {.error = agent_error(boost::beast::http::status::conflict, "agent_confirmation_expired", "Agent 操作确认已过期")};
    }
    tool_call->confirmed_at = now;
    if (!input.approved) {
        tool_call->status = "rejected";
        tool_call->finished_at = now;
        tool_call->result_json = boost::json::serialize(boost::json::object{{"result_text", "玩家已取消操作"}});
        *tool_call = repository_->save_tool_call(std::move(*tool_call));
        return {.value = AgentConfirmOutput{.tool_call = *tool_call, .result_text = "玩家已取消操作"}};
    }

    std::string result_text;
    std::string error_code;
    auto client_action = tool_registry_->execute_client_tool(*tool_call, result_text, error_code);
    tool_call->finished_at = now;
    if (!client_action) {
        tool_call->status = "failed";
        tool_call->error_code = error_code.empty() ? "agent_tool_forbidden" : error_code;
        *tool_call = repository_->save_tool_call(std::move(*tool_call));
        return {.error = agent_error(boost::beast::http::status::forbidden, tool_call->error_code, "Agent 工具未注册或参数不合法")};
    }
    tool_call->status = "succeeded";
    tool_call->result_json = boost::json::serialize(boost::json::object{
        {"result_text", result_text}, {"client_action", *client_action}});
    *tool_call = repository_->save_tool_call(std::move(*tool_call));
    return {.value = AgentConfirmOutput{
        .tool_call = *tool_call,
        .result_text = std::move(result_text),
        .client_action = std::move(client_action),
    }};
}

mf::game::ServiceResult<std::vector<AgentMessage>> AgentService::history(
    const std::string& player_id,
    const std::string& conversation_id,
    std::size_t limit) const {
    if (!repository_->find_conversation(conversation_id, player_id)) {
        return {.error = agent_error(boost::beast::http::status::not_found, "agent_conversation_not_found", "Agent 会话不存在")};
    }
    return {.value = repository_->list_messages(conversation_id, player_id, std::clamp<std::size_t>(limit, 1, 100))};
}

mf::game::ServiceResult<bool> AgentService::delete_conversation(
    const std::string& player_id,
    const std::string& conversation_id) const {
    if (!repository_->delete_conversation(conversation_id, player_id)) {
        return {.error = agent_error(boost::beast::http::status::not_found, "agent_conversation_not_found", "Agent 会话不存在")};
    }
    return {.value = true};
}

std::string AgentService::fallback_reply(const std::string& message, const std::string& context) const {
    if (message.find("背包") != std::string::npos || message.find("货币") != std::string::npos ||
        message.find("任务") != std::string::npos || message.find("邮件") != std::string::npos) {
        return "模型服务当前不可用，但后端已读取你的真实游戏数据。你可以让我打开对应面板查看详情。";
    }
    if (message.find("你好") != std::string::npos) return "你好，我是游戏助手。当前使用安全降级模式，仍可帮你打开面板或下达受控战术命令。";
    (void)context;
    return "AI 模型服务暂时不可用。当前已切换到后端安全降级模式，不会执行未经确认的操作。";
}

}  // namespace mf::agent
