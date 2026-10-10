#pragma once

// 文件职责：
// 暴露给 review 用的“存储架构导读”文本，方便快速理解为什么是 MySQL + Redis 分工。
// 调用流程：
// 开发者阅读源码或后续管理接口需要展示说明时 ->
// 调用 mysql_redis_review_notes() 获取结构化文本说明。

#include <string_view>

namespace mf::storage {

std::string_view mysql_redis_review_notes();

}  // namespace mf::storage 命名空间
