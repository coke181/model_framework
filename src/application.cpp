#include "mf/core/application.hpp"

// 文件职责：
// 实现 Application 的组装逻辑。
// 调用流程：
// main() 构造 Application -> route()/use() 注册路由和中间件 ->
// run() 记录启动日志 -> 创建 HttpServer -> 把请求处理正式交给 HTTP 层。

namespace mf::core {

Application::Application(AppOptions options)
    : options_(std::move(options)),
      logger_(make_console_logger()),
      middleware_chain_(std::make_shared<mf::http::MiddlewareChain>()),
      router_(std::make_shared<mf::http::Router>()) {
    // Router 在分发时读取共享中间件链，这样即使先注册路由、后注册中间件，
    // 也能保证所有路由看到的是一致的包装顺序。
    router_->set_middleware_chain(middleware_chain_);
}

void Application::route(
    mf::http::HttpMethod method,
    std::string path,
    mf::http::Handler handler) {
    router_->add_route(method, std::move(path), std::move(handler));
}

void Application::route_async(
    mf::http::HttpMethod method,
    std::string path,
    mf::http::AsyncHandler handler) {
    router_->add_async_route(method, std::move(path), std::move(handler));
}

void Application::use(mf::http::Middleware middleware) {
    middleware_chain_->add(std::move(middleware));
}

void Application::use_async(mf::http::AsyncMiddleware middleware) {
    middleware_chain_->add_async(std::move(middleware));
}

void Application::set_logger(std::shared_ptr<Logger> logger) {
    logger_ = std::move(logger);
}

const AppOptions& Application::options() const {
    return options_;
}

const std::shared_ptr<Logger>& Application::logger() const {
    return logger_;
}

void Application::run() {
    // Application 故意保持很薄，只负责顶层编排；
    // 传输层和领域层复杂度应留在各自组件中。
    logger_->log(LogLevel::info, "starting application " + options_.name);
    options_.server.logger = logger_;
    // 服务开始监听前先把路由中间件链预编译好，减少请求热路径上的 compose 开销。
    router_->prepare();
    mf::http::HttpServer server{options_.server, router_};
    server.run();
}

}  // namespace mf::core 命名空间
