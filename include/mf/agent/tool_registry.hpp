#pragma once

// 文件职责：集中维护 Agent 工具白名单、只读上下文和受控客户端动作。
// 第一阶段不注册资产修改、GM 或任意数据库工具。

#include "mf/agent/types.hpp"
#include "mf/game/repositories.hpp"
#include "mf/game/quest_service.hpp"

namespace mf::agent {

class AgentToolRegistry {
public:
    AgentToolRegistry(
        mf::game::RepositoryBundle repositories,
        std::shared_ptr<mf::game::QuestService> quest_service);

    std::string build_authoritative_context(const std::string& player_id) const;
    std::optional<AgentToolCall> suggest_tool(
        const std::string& player_id,
        const std::string& conversation_id,
        const std::string& message_id,
        const std::string& client_request_id,
        const std::string& user_message) const;
    std::optional<boost::json::object> execute_client_tool(
        const AgentToolCall& tool_call,
        std::string& result_text,
        std::string& error_code) const;
    bool is_registered(std::string_view tool_name) const;

private:
    mf::game::RepositoryBundle repositories_;
    std::shared_ptr<mf::game::QuestService> quest_service_;
};

}  // namespace mf::agent
