#pragma once

// 文件职责：
// 提供 GET query string 的通用解析辅助函数，避免各个 handler 重复拆字符串。
// 调用流程：
// handler 把 request.target() 传进来 ->
// 这里负责提取 key/value 并转换成 string / bool / size_t ->
// handler 再把结果交给业务层或仓储层。

#include <optional>
#include <string>
#include <string_view>

namespace mf::http {

// query_param 只负责取原始字符串；缺失参数时返回 nullopt，不主动报错。
std::optional<std::string> query_param(std::string_view target, std::string_view key);
// read_query_bool 会在参数存在但不是 true/false 时抛出明确的 ApiException。
std::optional<bool> read_query_bool(std::string_view target, std::string_view key);
bool read_query_bool_or(std::string_view target, std::string_view key, bool fallback);
// read_query_size 会在参数存在但不是合法非负整数时抛出明确的 ApiException。
std::size_t read_query_size(std::string_view target, std::string_view key, std::size_t fallback);

}  // namespace mf::http 命名空间
