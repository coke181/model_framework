#pragma once

// 文件职责：
// 提供一个聚合领域依赖的轻量服务入口，方便未来承载更复杂的跨仓储流程。
// 调用流程：
// 上层模块持有 GameService -> 通过它访问 repositories() 和 token_service() ->
// 后续如果领域逻辑变复杂，可把多步业务流程内聚到这里。

#include "mf/auth/token_service.hpp"
#include "mf/game/repositories.hpp"

#include <memory>

namespace mf::game {

class GameService {
public:
    // 这个包装类目前故意很小。
    // 等领域流程超过示例规模后，它会是聚合跨仓储业务编排的自然位置。
    GameService(RepositoryBundle repositories, std::shared_ptr<mf::auth::TokenService> token_service);

    const RepositoryBundle& repositories() const;
    const std::shared_ptr<mf::auth::TokenService>& token_service() const;

private:
    RepositoryBundle repositories_;
    std::shared_ptr<mf::auth::TokenService> token_service_;
};

}  // namespace mf::game 命名空间
