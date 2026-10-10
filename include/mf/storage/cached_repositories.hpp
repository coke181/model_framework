#pragma once

// 文件职责：
// 1. 提供“底层真实仓储 + 进程内只读快照/缓存”的包装层。
// 2. 优先覆盖权限、奖励模板这类读多写少数据，减少每次请求都回源 MySQL。
// 3. 对上层继续暴露原始 Repository 接口，尽量不改业务代码。
//
// 文件调用流程：
// 1. storage factory 先构造 MySQL 仓储。
// 2. 再按需用 cached repository 包一层。
// 3. 业务侧仍然通过 Repository 接口读写，但热点只读路径会先命中进程内缓存。

#include "mf/game/repositories.hpp"

#include <memory>

namespace mf::storage {

std::shared_ptr<mf::game::AdminRepository> make_cached_admin_repository(
    std::shared_ptr<mf::game::AdminRepository> inner);

std::shared_ptr<mf::game::RewardTemplateRepository> make_cached_reward_template_repository(
    std::shared_ptr<mf::game::RewardTemplateRepository> inner);

}  // namespace mf::storage
