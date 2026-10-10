#pragma once

// 文件职责：
// 提供常用 HTTP 响应构造函数，例如文本、JSON、404。
// 调用流程：
// 业务 handler 或 API 封装层调用这些函数 ->
// 生成带状态码、Content-Type 和 body 的标准 Response -> 交给 HttpServer 回写。

#include "mf/http/types.hpp"

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <string>

namespace mf::http {

inline Response make_text_response(
    beast_http::status status,
    std::string body,
    unsigned version = 11) {
    Response response{status, version};
    response.set(beast_http::field::content_type, "text/plain; charset=utf-8");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

inline Response make_html_response(
    beast_http::status status,
    std::string body,
    unsigned version = 11) {
    Response response{status, version};
    response.set(beast_http::field::content_type, "text/html; charset=utf-8");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

inline Response make_json_response(
    beast_http::status status,
    const boost::json::value& body,
    unsigned version = 11) {
    Response response{status, version};
    response.set(beast_http::field::content_type, "application/json; charset=utf-8");
    response.body() = boost::json::serialize(body);
    response.prepare_payload();
    return response;
}

inline Response make_not_found_response(unsigned version = 11) {
    // 默认 404 集中在这里，
    // 可以在框架尚未长出自定义异常或兜底路由机制前，先保持传输层行为一致。
    return make_text_response(beast_http::status::not_found, "route not found", version);
}

}  // namespace mf::http 命名空间
