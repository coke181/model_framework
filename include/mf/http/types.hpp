#pragma once

// 文件职责：
// 统一封装 HTTP 请求、响应和方法类型别名，减少上层直接接触 Beast 模板细节。
// 调用流程：
// Router、Middleware、Handler、Response 工具函数都引用这里的类型定义 ->
// 上层业务代码直接基于 Request/Response 编写接口逻辑。

#include <boost/beast/http.hpp>
#include <string_view>

namespace mf::http {

namespace beast_http = boost::beast::http;

// 第一版先把线协议体裁保持简单：入参和出参都用字符串 body。
// 流式传输、multipart、二进制负载可以后续再叠加。
using Request = beast_http::request<beast_http::string_body>;
using Response = beast_http::response<beast_http::string_body>;
using HttpMethod = beast_http::verb;

inline std::string_view method_name(HttpMethod method) {
    return beast_http::to_string(method);
}

}  // namespace mf::http 命名空间
