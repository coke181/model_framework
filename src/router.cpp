#include "mf/http/router.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现最小路由表和请求分发逻辑。
// 调用流程：
// Application 调用 add_route() 注册 handler ->
// HttpServer 收到请求后调用 dispatch() ->
// Router 先从 target 中截出 '?' 之前的 path，再查表命中 ->
// 套用中间件链 -> 执行业务 handler。

#include <boost/functional/hash.hpp>

namespace mf::http {

void Router::set_middleware_chain(std::shared_ptr<const MiddlewareChain> middleware_chain) {
    middleware_chain_ = std::move(middleware_chain);
    prepared_ = false;
    prepared_middleware_version_ = 0;
}

void Router::add_route(HttpMethod method, std::string path, Handler handler) {
    routes_.insert_or_assign(
        RouteKey{method, std::move(path)},
        RouteEntry{
            .raw_handler = std::move(handler),
            .prepared_handler = {},
            .raw_async_handler = {},
            .prepared_async_handler = {},
        });
    prepared_ = false;
}

void Router::add_async_route(HttpMethod method, std::string path, AsyncHandler handler) {
    routes_.insert_or_assign(
        RouteKey{method, std::move(path)},
        RouteEntry{
            .raw_handler = {},
            .prepared_handler = {},
            .raw_async_handler = std::move(handler),
            .prepared_async_handler = {},
        });
    prepared_ = false;
}

void Router::prepare() {
    const auto middleware_version = middleware_chain_ ? middleware_chain_->version() : 0;
    if (prepared_ && prepared_middleware_version_ == middleware_version) {
        return;
    }

    for (auto& [key, entry] : routes_) {
        static_cast<void>(key);
        if (entry.raw_handler && middleware_chain_) {
            entry.prepared_handler = middleware_chain_->compose(entry.raw_handler);
        } else if (entry.raw_handler) {
            entry.prepared_handler = entry.raw_handler;
        }
        if (entry.raw_async_handler && middleware_chain_) {
            entry.prepared_async_handler = middleware_chain_->compose_async(entry.raw_async_handler);
        } else if (entry.raw_async_handler) {
            entry.prepared_async_handler = entry.raw_async_handler;
        }
    }
    prepared_ = true;
    prepared_middleware_version_ = middleware_version;
}

std::optional<Response> Router::dispatch(const Request& request) const {
    MF_PROFILE_SCOPE("HTTP route and middleware");
    const auto key = make_route_key(request);
    if (const auto it = routes_.find(key); it != routes_.end()) {
        // dispatch 阶段只走 prepare() 预编译好的 handler，
        // 避免把中间件链折叠开销放进每个请求热路径。
        if (it->second.prepared_handler) {
            return it->second.prepared_handler(request);
        }
    }
    return std::nullopt;
}

bool Router::has_async_route(const Request& request) const {
    const auto it = routes_.find(make_route_key(request));
    return it != routes_.end() && static_cast<bool>(it->second.prepared_async_handler);
}

boost::asio::awaitable<std::optional<Response>> Router::dispatch_async(
    std::shared_ptr<const Request> request) const {
    MF_PROFILE_SCOPE("HTTP async route and middleware");
    const auto it = routes_.find(make_route_key(*request));
    if (it == routes_.end() || !it->second.prepared_async_handler) {
        co_return std::nullopt;
    }
    co_return co_await it->second.prepared_async_handler(*request);
}

Router::RouteKey Router::make_route_key(const Request& request) {
    // 路由表只关心路径部分，不关心 query string。
    const std::string_view target = request.target();
    const auto query_pos = target.find('?');
    const auto path =
        query_pos == std::string_view::npos ? std::string{target} : std::string{target.substr(0, query_pos)};
    return RouteKey{request.method(), path};
}

std::size_t Router::RouteKeyHash::operator()(const RouteKey& key) const {
    std::size_t seed = 0;
    boost::hash_combine(seed, static_cast<unsigned>(key.method));
    boost::hash_combine(seed, key.path);
    return seed;
}

}  // namespace mf::http 命名空间
