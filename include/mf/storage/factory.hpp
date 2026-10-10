#pragma once

// 文件职责：
// 定义仓储工厂入口，根据 StorageOptions 创建内存版或 MySQL + Redis 版仓储。
// 调用流程：
// main() 先加载 StorageOptions ->
// 调用 make_repositories() ->
// 若是 in_memory 则返回示例仓储，若是 mysql_redis 则构造真实存储仓储。

#include "mf/core/logger.hpp"
#include "mf/core/metrics.hpp"
#include "mf/game/repositories.hpp"
#include "mf/storage/options.hpp"

namespace mf::storage {

mf::game::RepositoryBundle make_repositories(
    const StorageOptions& options,
    const std::shared_ptr<mf::core::Logger>& logger,
    const std::shared_ptr<mf::core::MetricsRegistry>& metrics = {});

StorageOptions load_storage_options_from_env(
    std::string env_prefix,
    StorageOptions defaults = {});

}  // namespace mf::storage 命名空间
