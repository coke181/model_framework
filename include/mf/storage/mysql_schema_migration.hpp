#pragma once

// 文件职责：
// 定义 MySQL schema 自动迁移入口。
// 调用流程：
// make_repositories() 判断当前是 mysql_redis 模式 ->
// 调用 run_mysql_schema_migration_if_enabled() ->
// 自动创建数据库和缺失表/列 ->
// 再构造真正的 MySQL 仓储。

#include "mf/core/logger.hpp"
#include "mf/storage/options.hpp"

#include <memory>

namespace mf::storage {

// 如果 options.auto_migrate=true，则启动时执行 schema_file。
// schema SQL 必须保持幂等：CREATE TABLE IF NOT EXISTS、条件 ALTER TABLE。
void run_mysql_schema_migration_if_enabled(
    const MySqlOptions& options,
    const std::shared_ptr<mf::core::Logger>& logger);

}  // namespace mf::storage 命名空间
