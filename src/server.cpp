#include "mf/http/server.hpp"
#include "mf/core/api.hpp"
#include "mf/core/logger.hpp"
#include "mf/http/response_utils.hpp"
#include "mf/core/metrics.hpp"
#include "mf/core/profiling.hpp"

// 文件职责：
// 实现 HTTP 监听器、连接会话和请求读写循环。
// 调用流程：
// HttpServer::run() 启动 Listener ->
// Listener 接受 TCP 连接并创建 Session ->
// Session 读取 HTTP 请求 -> 调用 Router 分发 ->
// 写回 Response -> 若 keep-alive 则继续下一轮读取。

#include <boost/asio/dispatch.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mf::http {
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

std::string normalized_target(const Request& request) {
    const std::string_view target = request.target();
    const auto query_pos = target.find('?');
    return query_pos == std::string_view::npos ? std::string{target} : std::string{target.substr(0, query_pos)};
}

struct LoadController {
    explicit LoadController(const ServerOptions& options)
        : max_inflight_requests(std::max<std::size_t>(1, options.max_inflight_requests)),
          max_pending_business_tasks(std::max<std::size_t>(1, options.max_pending_business_tasks)),
          load_probe(options.load_probe ? options.load_probe : std::make_shared<LoadProbe>(
                                                           max_inflight_requests,
                                                           max_pending_business_tasks)) {
        load_probe->configure(max_inflight_requests, max_pending_business_tasks);
        load_probe->set_inflight_requests(0);
        load_probe->set_pending_business_tasks(0);
    }

    bool try_acquire_inflight() {
        auto current = inflight_requests.load(std::memory_order_relaxed);
        while (current < max_inflight_requests) {
            if (inflight_requests.compare_exchange_weak(
                    current,
                    current + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                load_probe->set_inflight_requests(current + 1);
                return true;
            }
        }
        return false;
    }

    void release_inflight() {
        const auto updated = inflight_requests.fetch_sub(1, std::memory_order_acq_rel) - 1;
        load_probe->set_inflight_requests(updated);
    }

    bool try_enqueue_business_task() {
        auto current = pending_business_tasks.load(std::memory_order_relaxed);
        while (current < max_pending_business_tasks) {
            if (pending_business_tasks.compare_exchange_weak(
                    current,
                    current + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                load_probe->set_pending_business_tasks(current + 1);
                return true;
            }
        }
        return false;
    }

    void mark_business_task_started() {
        const auto updated = pending_business_tasks.fetch_sub(1, std::memory_order_acq_rel) - 1;
        load_probe->set_pending_business_tasks(updated);
    }

    void note_rejected_too_many_inflight() {
        load_probe->increment_rejected_too_many_inflight();
    }

    void note_rejected_business_queue_full() {
        load_probe->increment_rejected_business_queue_full();
    }

    const std::size_t max_inflight_requests;
    const std::size_t max_pending_business_tasks;
    std::atomic<std::size_t> inflight_requests{0};
    std::atomic<std::size_t> pending_business_tasks{0};
    std::shared_ptr<LoadProbe> load_probe;
};

// 从 JSON body 里提取某个字符串字段，供传输层在鉴权前粗略地确定写串行 key。
// 这里刻意不做完整 JSON 解析，避免把每个写请求都引入一次 body 反序列化开销。
std::string json_string_field(std::string_view body, std::string_view field_name) {
    std::string needle;
    needle.reserve(field_name.size() + 2);
    needle.push_back('"');
    needle.append(field_name.data(), field_name.size());
    needle.push_back('"');

    const auto key_pos = body.find(needle);
    if (key_pos == std::string_view::npos) {
        return {};
    }
    const auto colon_pos = body.find(':', key_pos + needle.size());
    if (colon_pos == std::string_view::npos) {
        return {};
    }
    const auto value_pos = body.find('"', colon_pos + 1);
    if (value_pos == std::string_view::npos) {
        return {};
    }
    const auto value_end = body.find('"', value_pos + 1);
    if (value_end == std::string_view::npos) {
        return {};
    }
    return std::string{body.substr(value_pos + 1, value_end - value_pos - 1)};
}

// 这些写接口已经由数据库层的行锁、条件 UPDATE 或幂等 INSERT IGNORE 保证原子性，
// 同会话并发执行不会破坏一致性，因此不再按 session token 串行。
bool is_atomic_write_path(std::string_view target) {
    constexpr std::string_view paths[] = {
        "/players/me/inventory/item",
        "/shop/buy",
        "/loot/pickup",
        "/quests/accept",
        "/quests/events",
        "/quests/progress",
        "/quests/claim",
        "/mail/read",
        "/mail/claim",
        "/mail/delete",
        "/gacha/draw",
    };
    for (const auto path : paths) {
        if (target == path) {
            return true;
        }
    }
    return false;
}

// POST 但实际只读的接口，串行只会增加排队，直接放行。
bool is_post_read_path(std::string_view target) {
    constexpr std::string_view paths[] = {
        "/shop/history",
        "/gacha/history",
    };
    for (const auto path : paths) {
        if (target == path) {
            return true;
        }
    }
    return false;
}

// 从请求的 Authorization 头里提取会话 key。
// 写请求以该 key 做串行化，等价于“同一会话的写操作按到达顺序执行”。
// 登录/注册等还没有 Bearer token 的写请求则按 login_name 分 key，
// 避免所有未登录写请求都挤在同一个空 key 上被全局串行。
std::string session_key_from_request(const Request& request) {
    const auto target = normalized_target(request);
    if (is_atomic_write_path(target) || is_post_read_path(target)) {
        return {};
    }

    const auto header = request[boost::beast::http::field::authorization];
    if (!header.empty()) {
        const std::string_view value{header.data(), header.size()};
        constexpr std::string_view bearer_prefix = "Bearer ";
        if (value.size() >= bearer_prefix.size() && value.substr(0, bearer_prefix.size()) == bearer_prefix) {
            return std::string{value.substr(bearer_prefix.size())};
        }
        return std::string{value};
    }

    // 登录请求即使同一账号也允许并发：密码校验、风控和会话状态都落在 Redis/MySQL，
    // 单实例内不再为 login_name 排队，交给水平扩展后的多实例共同消化峰值。
    if (target == "/auth/login") {
        return {};
    }
    if (target == "/auth/register" || target == "/auth/quick-login" || target == "/admin/login") {
        const auto login_name = json_string_field(request.body(), "login_name");
        if (!login_name.empty()) {
            return "auth_login:" + login_name;
        }
    }
    return {};
}

bool is_mutating_method(boost::beast::http::verb method) {
    switch (method) {
    case boost::beast::http::verb::post:
    case boost::beast::http::verb::put:
    case boost::beast::http::verb::patch:
    case boost::beast::http::verb::delete_:
        return true;
    default:
        return false;
    }
}

// 按 key 串行的业务任务执行器。
// 每个 key 维护一个 FIFO 队列，同一时刻只放一个任务进业务线程池；
// 任务完成后从队首取下一个继续投递，从而保证同一 key 内任务顺序执行。
class KeyedBusinessExecutor {
public:
    KeyedBusinessExecutor(net::io_context& context, std::size_t max_per_key)
        : context_(context), max_per_key_(max_per_key) {}

    bool enqueue(std::string key, std::function<void()> task) {
        bool start = false;
        {
            std::scoped_lock lock{mutex_};
            auto& slot = slots_[key];
            if (slot.tasks.size() >= max_per_key_) {
                return false;
            }
            slot.tasks.push_back(std::move(task));
            if (!slot.active) {
                slot.active = true;
                start = true;
            }
        }
        if (start) {
            net::post(context_, [this, key = std::move(key)] {
                drain(key);
            });
        }
        return true;
    }

    std::size_t pending_key_count() const {
        std::scoped_lock lock{mutex_};
        return slots_.size();
    }

private:
    void drain(const std::string& key) {
        std::function<void()> task;
        {
            std::scoped_lock lock{mutex_};
            const auto it = slots_.find(key);
            if (it == slots_.end()) {
                return;
            }
            if (it->second.tasks.empty()) {
                slots_.erase(it);
                return;
            }
            task = std::move(it->second.tasks.front());
            it->second.tasks.pop_front();
        }

        try {
            task();
        } catch (...) {
            // 业务任务自身已经捕获异常并写回响应，这里兜底防止异常逃逸到 io_context。
        }

        net::post(context_, [this, key] {
            drain(key);
        });
    }

    struct Slot {
        std::deque<std::function<void()>> tasks;
        bool active = false;
    };

    net::io_context& context_;
    const std::size_t max_per_key_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> slots_;
};

}  // namespace

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(
        tcp::socket socket,
        std::shared_ptr<const Router> router,
        net::io_context& business_context,
        std::shared_ptr<LoadController> load_controller,
        std::shared_ptr<KeyedBusinessExecutor> keyed_executor,
        const ServerOptions& options)
        : socket_(std::move(socket)),
          router_(std::move(router)),
          business_context_(business_context),
          load_controller_(std::move(load_controller)),
          keyed_executor_(std::move(keyed_executor)),
          options_(options) {}

    void run() {
        net::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->read_request();
        });
    }

private:
    void reject_request(boost::beast::http::status status, std::string body, bool release_inflight_after_write) {
        record_rejected_request_metrics(status, body);
        Response response = make_text_response(status, std::move(body), request_.version());
        response.keep_alive(false);
        write_response(std::move(response), release_inflight_after_write);
    }

    void record_rejected_request_metrics(boost::beast::http::status status, const std::string& reason) {
        if (!options_.metrics) {
            return;
        }
        const auto duration_ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         std::chrono::steady_clock::now() - request_started_at_)
                                                         .count());
        const auto method = std::string{method_name(request_.method())};
        const auto path = normalized_target(request_);
        const auto status_text = std::to_string(static_cast<unsigned>(status));
        options_.metrics->increment_counter(
            "mf_http_requests_total",
            1.0,
            {
                {"method", method},
                {"path", path},
                {"status", status_text},
            });
        options_.metrics->observe_histogram_ms(
            "mf_http_request_duration_ms",
            duration_ms,
            {
                {"method", method},
                {"path", path},
            });
        options_.metrics->increment_counter(
            "mf_http_rejected_requests_total",
            1.0,
            {
                {"method", method},
                {"path", path},
                {"status", status_text},
                {"reason", reason},
            });
        if (duration_ms >= static_cast<double>(options_.slow_request_threshold_ms)) {
            options_.metrics->increment_counter(
                "mf_http_slow_requests_total",
                1.0,
                {
                    {"method", method},
                    {"path", path},
                    {"status", status_text},
                });
        }
    }

    void read_request() {
        // keep-alive 场景下复用同一个 session 对象，
        // 避免同一客户端每次 HTTP 调用都重新承担 accept/握手开销。
        request_ = {};
        boost::beast::http::async_read(
            socket_,
            buffer_,
            request_,
            [self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
                if (ec) {
                    return;
                }
                self->request_started_at_ = std::chrono::steady_clock::now();
                if (!self->load_controller_->try_acquire_inflight()) {
                    self->load_controller_->note_rejected_too_many_inflight();
                    self->reject_request(
                        boost::beast::http::status::too_many_requests,
                        "too many inflight requests",
                        false);
                    return;
                }
                try {
                    if (self->router_->has_async_route(self->request_)) {
                        self->dispatch_async_request();
                    } else {
                        self->dispatch_request();
                    }
                } catch (const mf::core::ApiException& api_exception) {
                    Response response = mf::core::make_error_response(api_exception.error(), self->request_.version());
                    response.keep_alive(false);
                    self->write_response(std::move(response), true);
                } catch (const std::exception&) {
                    // 传输层应始终把意外的 handler 异常转换成合法 HTTP 响应，
                    // 这样业务代码可以更简单，也避免连接被悄悄断掉。
                    Response response = make_text_response(
                        boost::beast::http::status::internal_server_error,
                        "internal server error",
                        self->request_.version());
                    response.keep_alive(false);
                    self->write_response(std::move(response), true);
                }
            });
    }

    void dispatch_async_request() {
        // 异步路由不占用业务线程池：协程在等待 Redis/MySQL 网络 I/O 时会挂起，
        // I/O 完成后再恢复执行，因此不会出现 future.get() 长时间占住 worker 的情况。
        auto request = std::make_shared<Request>(request_);
        net::co_spawn(
            socket_.get_executor(),
            [self = shared_from_this(), request = std::move(request)]() -> net::awaitable<void> {
                Response response;
                try {
                    response = (co_await self->router_->dispatch_async(request))
                                   .value_or(make_not_found_response(request->version()));
                    response.keep_alive(request->keep_alive());
                } catch (const mf::core::ApiException& api_exception) {
                    response = mf::core::make_error_response(api_exception.error(), request->version());
                    response.keep_alive(request->keep_alive());
                } catch (const std::exception& exception) {
                    if (self->options_.metrics) {
                        self->options_.metrics->increment_counter(
                            "mf_http_handler_exceptions_total", 1.0,
                            {{"method", std::string{method_name(request->method())}},
                             {"path", normalized_target(*request)}});
                    }
                    if (self->options_.logger) {
                        self->options_.logger->log(
                            mf::core::LogLevel::error,
                            "unhandled async request exception method=" +
                                std::string{method_name(request->method())} +
                                " path=" + normalized_target(*request) + " error=" + exception.what());
                    }
                    response = make_text_response(
                        boost::beast::http::status::internal_server_error,
                        "internal server error",
                        request->version());
                    response.keep_alive(false);
                }
                self->write_response(std::move(response), true);
            },
            net::detached);
    }

    void dispatch_request() {
        // 读包阶段尽快结束，把同步业务逻辑投递到独立线程池，
        // 避免数据库查询、JSON 拼装等工作长时间占住网络 worker。
        if (!load_controller_->try_enqueue_business_task()) {
            load_controller_->note_rejected_business_queue_full();
            reject_request(
                boost::beast::http::status::service_unavailable,
                "business queue is full",
                true);
            return;
        }
        const bool should_serialize =
            options_.serialize_mutating_requests && is_mutating_method(request_.method());
        const auto session_key = should_serialize ? session_key_from_request(request_) : std::string{};

        auto request = std::make_shared<Request>(request_);
        const auto enqueued_at = std::chrono::steady_clock::now();
        auto task = [self = shared_from_this(), request = std::move(request), enqueued_at]() mutable {
                MF_PROFILE_SCOPE("HTTP business task");
                self->load_controller_->mark_business_task_started();
#if MF_ENABLE_TRACY
                const auto queue_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - enqueued_at).count();
#endif
                Response response;
                try {
                    response = self->router_->dispatch(*request).value_or(make_not_found_response(request->version()));
                    response.keep_alive(request->keep_alive());
                } catch (const mf::core::ApiException& api_exception) {
                    response = mf::core::make_error_response(api_exception.error(), request->version());
                    response.keep_alive(request->keep_alive());
                } catch (const std::exception& exception) {
                    if (self->options_.metrics) {
                        self->options_.metrics->increment_counter(
                            "mf_http_requests_total", 1.0,
                            {{"method", std::string{method_name(request->method())}},
                             {"path", normalized_target(*request)}, {"status", "500"}});
                        self->options_.metrics->increment_counter(
                            "mf_http_handler_exceptions_total", 1.0,
                            {{"method", std::string{method_name(request->method())}},
                             {"path", normalized_target(*request)}});
                    }
                    if (self->options_.logger) {
                        self->options_.logger->log(
                            mf::core::LogLevel::error,
                            "unhandled request exception method=" + std::string{method_name(request->method())} +
                                " path=" + normalized_target(*request) + " error=" + exception.what());
                    }
                    response = make_text_response(
                        boost::beast::http::status::internal_server_error,
                        "internal server error",
                        request->version());
                    response.keep_alive(false);
                }
#if MF_ENABLE_TRACY
                response.set("X-Business-Queue-Ms", std::to_string(queue_ms));
#endif

                net::post(
                    self->socket_.get_executor(),
                    [self, response = std::move(response)]() mutable {
                        self->write_response(std::move(response), true);
                    });
            };

        if (should_serialize && !session_key.empty()) {
            if (!keyed_executor_->enqueue(session_key, std::move(task))) {
                load_controller_->mark_business_task_started();
                load_controller_->note_rejected_business_queue_full();
                reject_request(
                    boost::beast::http::status::service_unavailable,
                    "per-session write queue is full",
                    true);
            }
            return;
        }

        net::post(business_context_, std::move(task));
    }

    void write_response(Response response, bool release_inflight_after_write) {
#if MF_ENABLE_TRACY
        response.set("X-Server-Processing-Ms", std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - request_started_at_).count()));
#endif
        response_ = std::move(response);
        const auto response_write_started_at = std::chrono::steady_clock::now();

        boost::beast::http::async_write(
            socket_,
            response_,
            [self = shared_from_this(), release_inflight_after_write, response_write_started_at](
                boost::beast::error_code ec,
                std::size_t) {
                if (self->options_.metrics) {
                    self->options_.metrics->observe_histogram_ms(
                        "mf_http_response_write_duration_ms",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - response_write_started_at)
                            .count());
                }
                if (release_inflight_after_write) {
                    self->load_controller_->release_inflight();
                }
                if (ec) {
                    return;
                }
                if (!self->response_.keep_alive()) {
                    boost::beast::error_code ignored;
                    self->socket_.shutdown(tcp::socket::shutdown_send, ignored);
                    return;
                }
                self->read_request();
            });
    }

    tcp::socket socket_;
    std::shared_ptr<const Router> router_;
    net::io_context& business_context_;
    std::shared_ptr<LoadController> load_controller_;
    std::shared_ptr<KeyedBusinessExecutor> keyed_executor_;
    ServerOptions options_;
    boost::beast::flat_buffer buffer_;
    Request request_;
    Response response_;
    std::chrono::steady_clock::time_point request_started_at_{std::chrono::steady_clock::now()};
};

class HttpServer::Listener : public std::enable_shared_from_this<HttpServer::Listener> {
public:
    Listener(
        net::io_context& io_context,
        tcp::endpoint endpoint,
        std::shared_ptr<const Router> router,
        net::io_context& business_context,
        std::shared_ptr<LoadController> load_controller,
        std::shared_ptr<KeyedBusinessExecutor> keyed_executor,
        const ServerOptions& options)
        : io_context_(io_context),
          acceptor_(net::make_strand(io_context)),
          router_(std::move(router)),
          business_context_(business_context),
          load_controller_(std::move(load_controller)),
          keyed_executor_(std::move(keyed_executor)),
          options_(options) {
        boost::beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("failed to open acceptor");
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("failed to set reuse_address");
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("failed to bind endpoint");
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("failed to listen");
        }
    }

    void run() {
        // 保持多个 AcceptEx 同时挂起，连接突发时可以更快地排空内核监听队列。
        for (std::size_t index = 0; index < options_.accept_concurrency; ++index) {
            accept();
        }
    }

private:
    void accept() {
        acceptor_.async_accept(
            // 每个连接分配独立 strand。不能让所有 socket 复用 Listener 的同一个 strand，
            // 否则不同客户端的读写回调会被不必要地全局串行化。
            net::make_strand(io_context_),
            [self = shared_from_this()](boost::beast::error_code ec, tcp::socket socket) {
                // 先补回一个 accept 槽位，再处理刚收到的连接。
                self->accept();
                if (!ec) {
                    boost::beast::error_code option_ec;
                    socket.set_option(tcp::no_delay(true), option_ec);
                    // 每个新接入的 TCP 连接都会提升成一个 Session，
                    // 然后由这个 Session 独占该客户端的 HTTP 读写循环。
                    std::make_shared<Session>(
                        std::move(socket),
                        self->router_,
                        self->business_context_,
                        self->load_controller_,
                        self->keyed_executor_,
                        self->options_)
                        ->run();
                }
            });
    }

    net::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<const Router> router_;
    net::io_context& business_context_;
    std::shared_ptr<LoadController> load_controller_;
    std::shared_ptr<KeyedBusinessExecutor> keyed_executor_;
    ServerOptions options_;
};

HttpServer::HttpServer(ServerOptions options, std::shared_ptr<const Router> router)
    : options_(std::move(options)),
      router_(std::move(router)),
      io_context_(static_cast<int>(std::max<std::size_t>(1, options_.thread_count))) {
    options_.thread_count = std::max<std::size_t>(1, options_.thread_count);
    options_.business_thread_count = std::max<std::size_t>(1, options_.business_thread_count);
    if (options_.accept_concurrency == 0) {
        options_.accept_concurrency = std::clamp<std::size_t>(options_.thread_count * 2, 4, 64);
    }
}

void HttpServer::run() {
    const auto endpoint = tcp::endpoint{options_.host, options_.port};
    net::io_context business_context{static_cast<int>(options_.business_thread_count)};
    auto business_guard = net::make_work_guard(business_context);
    auto load_controller = std::make_shared<LoadController>(options_);
    auto keyed_executor = std::make_shared<KeyedBusinessExecutor>(
        business_context,
        std::max<std::size_t>(1, options_.max_pending_tasks_per_key));
    std::make_shared<Listener>(
        io_context_,
        endpoint,
        router_,
        business_context,
        load_controller,
        keyed_executor,
        options_)
        ->run();

    std::ostringstream startup_message;
    startup_message << "listening on " << endpoint.address().to_string() << ":" << endpoint.port()
                    << " with io_threads=" << options_.thread_count
                    << " accept_concurrency=" << options_.accept_concurrency
                    << " business_threads=" << options_.business_thread_count
                    << " max_inflight_requests=" << options_.max_inflight_requests
                    << " max_pending_business_tasks=" << options_.max_pending_business_tasks
                    << " serialize_mutating_requests=" << (options_.serialize_mutating_requests ? "on" : "off")
                    << " max_pending_tasks_per_key=" << options_.max_pending_tasks_per_key;
    if (options_.logger) {
        options_.logger->log(mf::core::LogLevel::info, startup_message.str());
    } else {
        std::cout << startup_message.str() << std::endl;
    }

    std::vector<std::jthread> workers;
    workers.reserve(options_.thread_count);
    std::vector<std::jthread> business_workers;
    business_workers.reserve(options_.business_thread_count);

    for (std::size_t i = 0; i < options_.thread_count; ++i) {
        // io_context 是共享事件循环。
        // 把它跑在多个 worker 线程上，可以让相互独立的 socket 并行推进。
        workers.emplace_back([this] {
            io_context_.run();
        });
    }

    for (std::size_t i = 0; i < options_.business_thread_count; ++i) {
        business_workers.emplace_back([&business_context] {
            business_context.run();
        });
    }
}

}  // namespace mf::http 命名空间
