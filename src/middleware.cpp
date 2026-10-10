#include "mf/http/middleware.hpp"

// 文件职责：
// 实现中间件链的注册和折叠逻辑。
// 调用流程：
// Application 调用 add() 注册中间件 ->
// Router 在 dispatch 时调用 compose() ->
// 生成“最外层中间件 -> ... -> 最终 handler”的执行流水线。

#include <utility>

namespace mf::http {

void MiddlewareChain::add(Middleware middleware) {
    middlewares_.push_back(std::move(middleware));
    version_.fetch_add(1, std::memory_order_relaxed);
}

void MiddlewareChain::add_async(AsyncMiddleware middleware) {
    async_middlewares_.push_back(std::move(middleware));
    version_.fetch_add(1, std::memory_order_relaxed);
}

Handler MiddlewareChain::compose(Handler terminal_handler) const {
    auto pipeline = std::move(terminal_handler);

    for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
        auto current = *it;
        auto next = pipeline;
        // 从里向外折叠，
        // 这样最先注册的中间件会成为最外层包装，符合大多数 Web 框架的执行顺序。
        pipeline = [current = std::move(current), next = std::move(next)](const Request& request) {
            return current(request, next);
        };
    }

    return pipeline;
}

AsyncHandler MiddlewareChain::compose_async(AsyncHandler terminal_handler) const {
    auto pipeline = std::move(terminal_handler);

    for (auto it = async_middlewares_.rbegin(); it != async_middlewares_.rend(); ++it) {
        auto current = *it;
        auto next = pipeline;
        pipeline = [current = std::move(current), next = std::move(next)](
                       const Request& request) -> boost::asio::awaitable<Response> {
            co_return co_await current(request, next);
        };
    }

    return pipeline;
}

std::size_t MiddlewareChain::version() const noexcept {
    return version_.load(std::memory_order_relaxed);
}

}  // namespace mf::http 命名空间
