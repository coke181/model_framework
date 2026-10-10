#include "mf/game/services.hpp"

// 文件职责：
// 实现 GameService 的简单访问器。
// 调用流程：
// 上层模块持有 GameService ->
// 通过 repositories()/token_service() 取出底层依赖 ->
// 后续可把复杂跨模块业务流程集中到这里。

namespace mf::game {

GameService::GameService(RepositoryBundle repositories, std::shared_ptr<mf::auth::TokenService> token_service)
    : repositories_(std::move(repositories)),
      token_service_(std::move(token_service)) {}

const RepositoryBundle& GameService::repositories() const {
    return repositories_;
}

const std::shared_ptr<mf::auth::TokenService>& GameService::token_service() const {
    // 目前 accessor 刻意保持简单；
    // 等服务层职责更清晰后，再把更复杂的领域流程收敛到 GameService 方法里。
    return token_service_;
}

}  // namespace mf::game 命名空间
