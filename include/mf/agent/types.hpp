#pragma once

// 文件职责：定义 Agent 配置、会话、消息、工具调用和接口返回对象。
// 安全边界：所有 player_id 都由后端登录 Session 注入，不接受 UE 自行指定。

#include <boost/json/object.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mf::agent {

struct AgentOptions {
    bool enabled = false;
    std::string provider = "openai_compatible";
    std::string api_endpoint;
    std::string api_key;
    std::string model = "deepseek-chat";
    std::chrono::seconds timeout{12};
    std::size_t max_message_length = 2000;
    std::size_t max_history_messages = 12;
    std::size_t max_context_bytes = 8192;
    std::int64_t daily_request_limit = 200;
    std::chrono::seconds confirmation_ttl{60};
};

struct AgentConversation {
    std::string conversation_id;
    std::string player_id;
    std::string scene;
    std::string title;
    std::string status = "active";
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> updated_at;
};

struct AgentMessage {
    std::string message_id;
    std::string conversation_id;
    std::string player_id;
    std::string client_request_id;
    std::string role;
    std::string content;
    std::string model;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::optional<std::chrono::system_clock::time_point> created_at;
};

struct AgentToolCall {
    std::string call_id;
    std::string conversation_id;
    std::string message_id;
    std::string player_id;
    std::string tool_name;
    std::string arguments_json;
    std::string risk_level;
    std::string status;
    std::string display_text;
    std::string result_json;
    std::string error_code;
    std::string idempotency_key;
    std::optional<std::chrono::system_clock::time_point> created_at;
    std::optional<std::chrono::system_clock::time_point> confirmed_at;
    std::optional<std::chrono::system_clock::time_point> finished_at;
};

struct AgentUsageDaily {
    std::string player_id;
    std::string usage_date;
    std::int64_t request_count = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t failed_count = 0;
};

struct AgentChatInput {
    std::string conversation_id;
    std::string client_request_id;
    std::string message;
    std::string scene;
    std::string client_context_json;
};

struct AgentChatOutput {
    std::string conversation_id;
    std::string message_id;
    std::string reply;
    bool used_fallback = false;
    std::vector<AgentToolCall> tool_calls;
    std::vector<boost::json::object> suggested_actions;
};

struct AgentConfirmInput {
    std::string conversation_id;
    std::string call_id;
    std::string client_request_id;
    bool approved = false;
};

struct AgentConfirmOutput {
    AgentToolCall tool_call;
    std::string result_text;
    std::optional<boost::json::object> client_action;
};

struct AgentModelRequest {
    std::string system_prompt;
    std::vector<AgentMessage> history;
    std::string user_message;
    std::string authoritative_context;
    std::string client_context;
};

struct AgentModelResponse {
    bool success = false;
    std::string content;
    std::string error_code;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
};

AgentOptions load_agent_options_from_env(AgentOptions defaults = {});

}  // namespace mf::agent
