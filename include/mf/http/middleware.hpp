#pragma once

// 文件职责：
// 定义 HTTP handler、中间件以及中间件链的抽象。
// 调用流程：
// Application 注册 middleware -> Router 在 dispatch 时调用 compose() ->
// 中间件一层层包住最终 handler -> Request 依次流过各层并产出 Response。

#include "mf/http/types.hpp"

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <vector>

namespace mf::http {

// Handler 表示最终把 Request 转成 Response 的同步处理函数。
using Handler = std::function<Response(const Request&)>;
// Middleware 是包裹层，可以在调用 next() 之前做检查、增强或直接短路返回。
using Middleware = std::function<Response(const Request&, const Handler&)>;

// AsyncHandler 用于真正会挂起等待 I/O 的路由。
// Request 的生命周期由 Router::dispatch_async() 的协程帧持有，因此异步处理期间引用始终有效。
using AsyncHandler = std::function<boost::asio::awaitable<Response>(const Request&)>;
// 异步中间件与同步中间件语义一致，但 next() 可以 co_await，避免阻塞 HTTP 或业务线程。
using AsyncMiddleware =
    std::function<boost::asio::awaitable<Response>(const Request&, const AsyncHandler&)>;

class MiddlewareChain {
public:
    void add(Middleware middleware);
    void add_async(AsyncMiddleware middleware);
    // compose() 把当前中间件栈真正包到一个终端 handler 外面。
    Handler compose(Handler terminal_handler) const;
    AsyncHandler compose_async(AsyncHandler terminal_handler) const;
    std::size_t version() const noexcept;

private:
    std::vector<Middleware> middlewares_;
    std::vector<AsyncMiddleware> async_middlewares_;
    std::atomic<std::size_t> version_{0};
};

}  // namespace mf::http 命名空间
