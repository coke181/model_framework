#pragma once

// 文件职责：封装 OpenAI-compatible 模型供应商差异，API Key 只在后端进程内使用。

#include "mf/agent/types.hpp"

#include <memory>

namespace mf::agent {

class AgentModelProvider {
public:
    virtual ~AgentModelProvider() = default;
    virtual AgentModelResponse complete(const AgentModelRequest& request) const = 0;
};

std::shared_ptr<AgentModelProvider> make_agent_model_provider(const AgentOptions& options);

}  // namespace mf::agent
