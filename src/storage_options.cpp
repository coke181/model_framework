#include "mf/storage/options.hpp"

// 文件职责：
// 实现存储模式字符串转换。
// 调用流程：
// 配置层或诊断接口拿到 StorageMode ->
// 调用 to_string() 输出当前运行使用的是内存模式还是 MySQL + Redis 模式。

namespace mf::storage {

const char* to_string(StorageMode mode) {
    switch (mode) {
    case StorageMode::in_memory:
        return "in_memory";
    case StorageMode::mysql_redis:
        return "mysql_redis";
    }
    return "unknown";
}

}  // namespace mf::storage 命名空间
