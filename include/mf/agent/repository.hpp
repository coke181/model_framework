#pragma once

// 文件职责：隔离 Agent 会话、消息、工具审计和每日用量的内存/MySQL 实现。

#include "mf/agent/types.hpp"

namespace mf::agent {

class AgentRepository {
public:
    virtual ~AgentRepository() = default;

    virtual AgentConversation save_conversation(AgentConversation conversation) = 0;
    virtual std::optional<AgentConversation> find_conversation(
        const std::string& conversation_id,
        const std::string& player_id) const = 0;
    virtual bool delete_conversation(
        const std::string& conversation_id,
        const std::string& player_id) = 0;

    virtual AgentMessage append_message(AgentMessage message) = 0;
    virtual std::vector<AgentMessage> list_messages(
        const std::string& conversation_id,
        const std::string& player_id,
        std::size_t limit) const = 0;
    virtual std::optional<AgentMessage> find_assistant_message_by_request(
        const std::string& player_id,
        const std::string& client_request_id) const = 0;

    virtual AgentToolCall save_tool_call(AgentToolCall tool_call) = 0;
    virtual std::optional<AgentToolCall> find_tool_call(
        const std::string& call_id,
        const std::string& conversation_id,
        const std::string& player_id) const = 0;
    virtual std::vector<AgentToolCall> list_tool_calls_for_message(const std::string& message_id) const = 0;

    virtual AgentUsageDaily get_daily_usage(const std::string& player_id, const std::string& usage_date) const = 0;
    virtual AgentUsageDaily record_usage(
        const std::string& player_id,
        const std::string& usage_date,
        std::int64_t prompt_tokens,
        std::int64_t completion_tokens,
        bool failed) = 0;
};

}  // namespace mf::agent
