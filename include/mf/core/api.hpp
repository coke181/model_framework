#pragma once

// 文件职责：
// 定义 HTTP API 层通用的成功/失败响应结构，以及 JSON 请求体解析入口。
// 调用流程：
// 业务 handler 收到 Request -> 调用 parse_json_body() 解析请求 ->
// 业务处理完成后调用 make_ok_response()/make_error_response() 返回统一响应。

#include "mf/http/response_utils.hpp"

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>

namespace mf::core {

struct ApiError {
    // ApiError 是面向 HTTP handler 的传输层错误结构。
    // 领域层内部可以演进出更丰富的错误类型，再在边界映射到这里。
    boost::beast::http::status status = boost::beast::http::status::bad_request;
    std::string code;
    std::string message;
    boost::json::object details;
};

class ApiException : public std::runtime_error {
public:
    explicit ApiException(ApiError error);

    const ApiError& error() const noexcept;

private:
    ApiError error_;
};

// 用统一的 "ok/data" 包裹结构，便于客户端对不同接口做一致处理。
boost::json::object ok_payload(boost::json::value data = {});
boost::json::object error_payload(
    std::string_view code,
    std::string_view message,
    const boost::json::object& details = {});
mf::http::Response make_ok_response(
    const boost::json::value& data,
    unsigned version = 11,
    boost::beast::http::status status = boost::beast::http::status::ok);
mf::http::Response make_error_response(
    const ApiError& error,
    unsigned version = 11);
// parse_json_body 把 JSON 语法校验集中起来，
// 让 handler 更专注于业务字段，而不是传输层解析细节。
std::optional<boost::json::value> parse_json_body(const mf::http::Request& request, ApiError& error);

}  // namespace mf::core 命名空间
