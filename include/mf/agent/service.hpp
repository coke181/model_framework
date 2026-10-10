#pragma once

// 文件职责：执行 Agent 主流程、登录玩家绑定、限流、模型调用、确认和审计。

#include "mf/agent/model_provider.hpp"
#include "mf/agent/repository.hpp"
#include "mf/agent/tool_registry.hpp"
#include "mf/game/mail_service.hpp"

namespace mf::agent {

class AgentService {
public:
    AgentService(
        AgentOptions options,
        std::shared_ptr<AgentRepository> repository,
        std::shared_ptr<AgentModelProvider> model_provider,
        std::shared_ptr<AgentToolRegistry> tool_registry);

    mf::game::ServiceResult<AgentChatOutput> chat(
        const std::string& player_id,
        const std::string& display_name,
        AgentChatInput input) const;
    mf::game::ServiceResult<AgentConfirmOutput> confirm(
        const std::string& player_id,
        AgentConfirmInput input) const;
    mf::game::ServiceResult<std::vector<AgentMessage>> history(
        const std::string& player_id,
        const std::string& conversation_id,
        std::size_t limit) const;
    mf::game::ServiceResult<bool> delete_conversation(
        const std::string& player_id,
        const std::string& conversation_id) const;

private:
    std::string fallback_reply(const std::string& message, const std::string& context) const;

    AgentOptions options_;
    std::shared_ptr<AgentRepository> repository_;
    std::shared_ptr<AgentModelProvider> model_provider_;
    std::shared_ptr<AgentToolRegistry> tool_registry_;
};

}  // namespace mf::agent
