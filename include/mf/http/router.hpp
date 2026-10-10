#pragma once

// 文件职责：
// 定义最基础的 method + path 路由表，以及请求分发入口。
// 调用流程：
// Application 调用 add_route() 注册路由 -> HttpServer 收到请求后调用 dispatch() ->
// Router 查找匹配 handler -> 按需套上中间件链 -> 返回最终 Response。

#include "mf/http/middleware.hpp"
#include "mf/http/types.hpp"

#include <optional>
#include <memory>
#include <string>
#include <unordered_map>

namespace mf::http {

class Router {
public:
    // 当前 Router 先保持“精确路径匹配”。
    // 以后即使加路径参数，也不必改变 handler 接口本身。
    void set_middleware_chain(std::shared_ptr<const MiddlewareChain> middleware_chain);
    void add_route(HttpMethod method, std::string path, Handler handler);
    void add_async_route(HttpMethod method, std::string path, AsyncHandler handler);
    // prepare() 在服务正式监听前把中间件流水线预编译到每条路由上，
    // 避免每个请求都重复 compose 一次 handler 链。
    void prepare();
    std::optional<Response> dispatch(const Request& request) const;
    bool has_async_route(const Request& request) const;
    boost::asio::awaitable<std::optional<Response>> dispatch_async(
        std::shared_ptr<const Request> request) const;

private:
    struct RouteKey {
        HttpMethod method;
        std::string path;

        bool operator==(const RouteKey& other) const = default;
    };

    struct RouteKeyHash {
        std::size_t operator()(const RouteKey& key) const;
    };

    struct RouteEntry {
        Handler raw_handler;
        Handler prepared_handler;
        AsyncHandler raw_async_handler;
        AsyncHandler prepared_async_handler;
    };

    static RouteKey make_route_key(const Request& request);

    std::shared_ptr<const MiddlewareChain> middleware_chain_;
    bool prepared_ = false;
    std::size_t prepared_middleware_version_ = 0;
    std::unordered_map<RouteKey, RouteEntry, RouteKeyHash> routes_;
};

}  // namespace mf::http 命名空间
