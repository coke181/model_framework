#include "mf/core/api.hpp"

// 文件职责：
// 实现统一 API 响应包裹和 JSON 请求体解析。
// 调用流程：
// handler 先调用 parse_json_body() 解析 body ->
// 成功时调用 make_ok_response()，失败时调用 make_error_response() ->
// 返回统一的 HTTP JSON 响应。

#include <boost/json/parse.hpp>
#include <chrono>
#include <exception>

namespace mf::core {

ApiException::ApiException(ApiError error)
    : std::runtime_error(error.message.empty() ? error.code : error.message),
      error_(std::move(error)) {}

const ApiError& ApiException::error() const noexcept {
    return error_;
}

boost::json::object ok_payload(boost::json::value data) {
    return {
        {"ok", true},
        {"meta",
         {
             {"protocol_version", "v1"},
             {"server_time", std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count()},
         }},
        {"data", std::move(data)},
    };
}

boost::json::object error_payload(
    std::string_view code,
    std::string_view message,
    const boost::json::object& details) {
    return {
        {"ok", false},
        {"meta",
         {
             {"protocol_version", "v1"},
             {"server_time", std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count()},
         }},
        {"error",
         {
             {"code", code},
             {"message", message},
             {"details", details},
         }},
    };
}

mf::http::Response make_ok_response(
    const boost::json::value& data,
    unsigned version,
    boost::beast::http::status status) {
    // 把响应包裹结构集中在这里，
    // 可以避免每个 handler 重复拼响应格式，客户端一旦依赖协议也更稳定。
    return mf::http::make_json_response(status, ok_payload(data), version);
}

mf::http::Response make_error_response(const ApiError& error, unsigned version) {
    return mf::http::make_json_response(
        error.status,
        error_payload(error.code, error.message, error.details),
        version);
}

std::optional<boost::json::value> parse_json_body(const mf::http::Request& request, ApiError& error) {
    try {
        auto parsed = boost::json::parse(request.body());
        if (!parsed.is_object()) {
            error = ApiError{
                .status = boost::beast::http::status::bad_request,
                .code = "invalid_json_root_type",
                .message = "请求体顶层必须是 JSON 对象",
                .details = {{"expected_type", "object"}},
            };
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        // JSON 语法错误在入口边界统一转换，
        // 这样 handler 只要拿到 body，就可以默认它已经是可用的解析结果。
        error = ApiError{
            .status = boost::beast::http::status::bad_request,
            .code = "invalid_json",
            .message = "请求体不是合法的 JSON",
            .details = {},
        };
        return std::nullopt;
    }
}

}  // namespace mf::core 命名空间
