#include "mf/http/query_params.hpp"
#include "mf/core/api.hpp"

// 文件职责：
// 实现 GET query string 的基础解析逻辑。
// 调用流程：
// handler 调用 query_param/read_query_* ->
// 这里从 target 中切出 '?' 后的查询串并做类型转换 ->
// 把解析结果返回给上层。

#include <cstdlib>
#include <limits>

namespace mf::http {
namespace {

[[noreturn]] void throw_query_error(
    std::string_view code,
    std::string_view message,
    std::string_view key,
    std::string_view value,
    boost::json::object details = {}) {
    details["field"] = std::string(key);
    details["location"] = "query";
    details["provided_value"] = std::string(value);
    throw mf::core::ApiException(mf::core::ApiError{
        .status = boost::beast::http::status::bad_request,
        .code = std::string(code),
        .message = std::string(message),
        .details = std::move(details),
    });
}

}  // namespace

std::optional<std::string> query_param(std::string_view target, std::string_view key) {
    const auto pos = target.find('?');
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }

    auto query = target.substr(pos + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        const auto current_key = pair.substr(0, eq);
        const auto current_value = eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
        if (current_key == key) {
            return std::string{current_value};
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

std::optional<bool> read_query_bool(std::string_view target, std::string_view key) {
    const auto value = query_param(target, key);
    if (!value) {
        return std::nullopt;
    }
    if (*value == "true") {
        return true;
    }
    if (*value == "false") {
        return false;
    }
    throw_query_error(
        "invalid_query_bool",
        "查询参数只能是 true 或 false",
        key,
        *value,
        {
            {"expected_values", boost::json::array{"true", "false"}},
            {"expected_value_labels", boost::json::array{"是", "否"}},
        });
}

bool read_query_bool_or(std::string_view target, std::string_view key, bool fallback) {
    return read_query_bool(target, key).value_or(fallback);
}

std::size_t read_query_size(std::string_view target, std::string_view key, std::size_t fallback) {
    if (const auto value = query_param(target, key)) {
        if (value->empty()) {
            throw_query_error(
                "invalid_query_size",
                "查询参数必须是非负整数",
                key,
                *value);
        }
        std::size_t parsed = 0;
        for (const char ch : *value) {
            if (ch < '0' || ch > '9') {
                throw_query_error(
                    "invalid_query_size",
                    "查询参数必须是非负整数",
                    key,
                    *value);
            }
            const auto digit = static_cast<std::size_t>(ch - '0');
            if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
                throw_query_error(
                    "query_size_out_of_range",
                    "查询参数数值过大",
                    key,
                    *value);
            }
            parsed = parsed * 10 + digit;
        }
        return parsed;
    }
    return fallback;
}

}  // namespace mf::http 命名空间
