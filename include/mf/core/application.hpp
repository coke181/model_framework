#pragma once

// 文件职责：
// 定义应用顶层入口，负责把路由、中间件、日志和 HTTP 服务组装起来。
// 调用流程：
// main() 构造 Application -> 注册 middleware/route -> 调用 run() ->
// Application 创建 HttpServer -> HttpServer 开始监听并分发请求。

#include "mf/http/router.hpp"
#include "mf/http/server.hpp"
#include "mf/http/middleware.hpp"

#include "mf/core/logger.hpp"

#include <memory>
#include <string>

namespace mf::core {

struct AppOptions {
    // AppOptions 只收拢进程级横切配置。
    // 各功能模块自己的配置应独立建模，而不是不断往这里堆字段。
    std::string name = "model-service";
    mf::http::ServerOptions server;
};

class Application {
public:
    explicit Application(AppOptions options = {});

    // 路由注册面向传输层，故意保持低样板。
    // 如果以后需要，更高层的 controller 风格抽象仍然可以叠加在上面。
    void route(mf::http::HttpMethod method, std::string path, mf::http::Handler handler);
    void route_async(mf::http::HttpMethod method, std::string path, mf::http::AsyncHandler handler);
    // 中间件会包住所有路由，
    // 是放鉴权、日志、指标、限流、统一错误转换这类横切逻辑的正确位置。
    void use(mf::http::Middleware middleware);
    void use_async(mf::http::AsyncMiddleware middleware);
    void set_logger(std::shared_ptr<Logger> logger);
    const AppOptions& options() const;
    const std::shared_ptr<Logger>& logger() const;
    // run() 会阻塞在 HTTP server 的事件循环上。
    // 像 chat gateway 这样的长生命周期侧车，应在调用它之前启动。
    void run();

private:
    AppOptions options_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<mf::http::MiddlewareChain> middleware_chain_;
    std::shared_ptr<mf::http::Router> router_;
};

}  // namespace mf::core 命名空间
