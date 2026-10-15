#include "mf/chat/gateway.hpp"
#include "mf/core/api.hpp"

// 文件职责：
// 实现 WebSocket 聊天网关，包括监听器和单连接会话状态机。
// 调用流程：
// ChatGateway::run() 开始监听 -> Listener 接收 TCP 连接 ->
// 创建 GatewaySession -> 完成 websocket 握手 -> 读取消息 ->
// 根据 type 分发到 auth/join/say/history -> 必要时通过 ChatService 广播。

#include "mf/chat/channels.hpp"
#include "mf/game/json.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value_to.hpp>

#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mf::chat {
namespace net = boost::asio;
namespace websocket = boost::beast::websocket;
using tcp = net::ip::tcp;

namespace {

[[noreturn]] void throw_ws_error(
    std::string_view code,
    std::string_view message,
    std::string_view field = {},
    boost::json::object details = {}) {
    if (!field.empty()) {
        details["field"] = std::string(field);
    }
    details["location"] = "websocket_payload";
    throw mf::core::ApiException(mf::core::ApiError{
        .status = boost::beast::http::status::bad_request,
        .code = std::string(code),
        .message = std::string(message),
        .details = std::move(details),
    });
}

std::string json_kind_name(const boost::json::value& value) {
    switch (value.kind()) {
    case boost::json::kind::null:
        return "null";
    case boost::json::kind::bool_:
        return "bool";
    case boost::json::kind::int64:
        return "int64";
    case boost::json::kind::uint64:
        return "uint64";
    case boost::json::kind::double_:
        return "double";
    case boost::json::kind::string:
        return "string";
    case boost::json::kind::array:
        return "array";
    case boost::json::kind::object:
        return "object";
    }
    return "unknown";
}

const boost::json::value* require_ws_field(const boost::json::object& object, std::string_view key) {
    if (const auto* value = object.if_contains(key)) {
        return value;
    }
    throw_ws_error("missing_required_field", "缺少必填字段", key);
}

std::string require_ws_string(const boost::json::object& object, std::string_view key) {
    const auto* value = require_ws_field(object, key);
    if (!value->is_string()) {
        throw_ws_error(
            "invalid_field_type",
            "字段类型不正确",
            key,
            {
                {"expected_type", "string"},
                {"expected_type_label", "字符串"},
                {"actual_type", json_kind_name(*value)},
            });
    }
    return boost::json::value_to<std::string>(*value);
}

std::string read_optional_ws_string(const boost::json::object& object, std::string_view key, std::string fallback = {}) {
    if (const auto* value = object.if_contains(key)) {
        if (!value->is_string()) {
            throw_ws_error(
                "invalid_field_type",
                "字段类型不正确",
                key,
                {
                    {"expected_type", "string"},
                    {"expected_type_label", "字符串"},
                    {"actual_type", json_kind_name(*value)},
                });
        }
        return boost::json::value_to<std::string>(*value);
    }
    return fallback;
}

std::size_t read_optional_ws_size(const boost::json::object& object, std::string_view key, std::size_t fallback) {
    if (const auto* value = object.if_contains(key)) {
        if (value->is_int64()) {
            const auto parsed = value->as_int64();
            if (parsed < 0) {
                throw_ws_error(
                    "invalid_field_value",
                    "字段值不合法",
                    key,
                    {{"expected", "non_negative_integer"}, {"expected_label", "非负整数"}});
            }
            return static_cast<std::size_t>(parsed);
        }
        if (value->is_uint64()) {
            return static_cast<std::size_t>(value->as_uint64());
        }
        throw_ws_error(
            "invalid_field_type",
            "字段类型不正确",
            key,
            {
                {"expected_type", "int64"},
                {"expected_type_label", "整数"},
                {"actual_type", json_kind_name(*value)},
            });
    }
    return fallback;
}

boost::json::object ws_error_payload(const mf::core::ApiError& error) {
    return {
        {"type", "error"},
        {"code", error.code},
        {"message", error.message},
        {"details", error.details},
    };
}

}  // namespace

namespace {

struct GatewayRuntimeStats {
    std::shared_ptr<mf::core::MetricsRegistry> metrics;
    std::atomic<std::size_t> active_sessions{0};
};

class MetricsHttpSession : public std::enable_shared_from_this<MetricsHttpSession> {
public:
    MetricsHttpSession(tcp::socket socket, std::shared_ptr<mf::core::MetricsRegistry> metrics)
        : socket_(std::move(socket)), metrics_(std::move(metrics)) {}

    void run() {
        boost::beast::http::async_read(
            socket_, buffer_, request_, [self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
                if (ec) {
                    return;
                }
                self->respond();
            });
    }

private:
    void respond() {
        const bool is_metrics = request_.method() == boost::beast::http::verb::get && request_.target() == "/metrics";
        const bool is_health = request_.method() == boost::beast::http::verb::get && request_.target() == "/health";
        const auto status = is_metrics || is_health
                                ? boost::beast::http::status::ok
                                : boost::beast::http::status::not_found;
        response_.emplace(status, request_.version());
        response_->set(boost::beast::http::field::server, "mf-chat-gateway");
        response_->set(
            boost::beast::http::field::content_type,
            is_metrics ? "text/plain; version=0.0.4; charset=utf-8" : "text/plain; charset=utf-8");
        response_->keep_alive(false);
        response_->body() = is_metrics && metrics_ ? metrics_->render_prometheus()
                              : is_health               ? "ok\n"
                                                        : "not found\n";
        response_->prepare_payload();
        boost::beast::http::async_write(
            socket_, *response_, [self = shared_from_this()](boost::beast::error_code, std::size_t) {
                boost::system::error_code ignored;
                self->socket_.shutdown(tcp::socket::shutdown_send, ignored);
                self->socket_.close(ignored);
            });
    }

    tcp::socket socket_;
    boost::beast::flat_buffer buffer_;
    boost::beast::http::request<boost::beast::http::string_body> request_;
    std::optional<boost::beast::http::response<boost::beast::http::string_body>> response_;
    std::shared_ptr<mf::core::MetricsRegistry> metrics_;
};

class MetricsHttpListener : public std::enable_shared_from_this<MetricsHttpListener> {
public:
    MetricsHttpListener(
        net::io_context& io_context,
        tcp::endpoint endpoint,
        std::shared_ptr<mf::core::MetricsRegistry> metrics)
        : acceptor_(net::make_strand(io_context)),
          socket_(net::make_strand(io_context)),
          metrics_(std::move(metrics)) {
        boost::beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) throw std::runtime_error("failed to open chat metrics acceptor");
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) throw std::runtime_error("failed to set chat metrics reuse_address");
        acceptor_.bind(endpoint, ec);
        if (ec) throw std::runtime_error("failed to bind chat metrics endpoint");
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) throw std::runtime_error("failed to listen on chat metrics endpoint");
    }

    void run() { accept(); }

private:
    void accept() {
        acceptor_.async_accept(socket_, [self = shared_from_this()](boost::beast::error_code ec) {
            if (!ec) {
                std::make_shared<MetricsHttpSession>(std::move(self->socket_), self->metrics_)->run();
            }
            self->accept();
        });
    }

    tcp::acceptor acceptor_;
    tcp::socket socket_;
    std::shared_ptr<mf::core::MetricsRegistry> metrics_;
};

void set_gateway_active_sessions(const std::shared_ptr<GatewayRuntimeStats>& stats, std::size_t value) {
    if (stats && stats->metrics) {
        stats->metrics->set_gauge("mf_chat_gateway_active_sessions", static_cast<double>(value));
    }
}

}  // namespace

class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
public:
    GatewaySession(
        tcp::socket socket,
        GatewayOptions options,
        std::shared_ptr<GatewayRuntimeStats> stats,
        std::shared_ptr<ChatService> chat_service,
        std::shared_ptr<ChatStateService> state_service,
        std::shared_ptr<mf::auth::TokenService> token_service,
        std::shared_ptr<mf::core::Logger> logger)
        : websocket_(std::move(socket)),
          options_(std::move(options)),
          stats_(std::move(stats)),
          chat_service_(std::move(chat_service)),
          state_service_(std::move(state_service)),
          token_service_(std::move(token_service)),
          logger_(std::move(logger)) {}

    void run() {
        // WebSocket 握手和业务鉴权是两层概念：
        // 先接受套接字，再由上层协议用 token 做身份认证。
        websocket_.set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
        websocket_.read_message_max(options_.max_message_bytes);
        websocket_.async_accept([self = shared_from_this()](boost::beast::error_code ec) {
            if (ec) {
                self->close_transport();
                return;
            }
            self->read();
        });
    }

private:
    void read() {
        websocket_.async_read(
            buffer_,
            [self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
                if (ec) {
                    self->close_transport();
                    return;
                }
                self->handle_message();
            });
    }

    void handle_message() {
        const auto text = boost::beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        try {
            // 这里故意不用“泛 RPC”式分发，
            // 而是显式按消息类型路由，方便早期开发时直接观察线协议。
            const auto parsed = boost::json::parse(text);
            if (!parsed.is_object()) {
                throw_ws_error(
                    "invalid_json_root_type",
                    "WebSocket 消息顶层必须是 JSON 对象",
                    {},
                    {{"expected_type", "object"}});
            }
            const auto payload = parsed.as_object();
            const auto type = require_ws_string(payload, "type");

            if (type != "auth" && !validate_session()) {
                // validate_session 已返回协议错误；保持连接以允许重新鉴权。
            } else if (type == "auth") {
                handle_auth(payload);
            } else if (type == "join") {
                handle_join(payload);
            } else if (type == "say") {
                handle_say(payload);
            } else if (type == "recall") {
                handle_recall(payload);
            } else if (type == "history") {
                handle_history(payload);
            } else {
                send_json({
                    {"type", "error"},
                    {"code", "unknown_message_type"},
                    {"message", "未知的消息类型"},
                    {"details", boost::json::object{{"provided_type", type}}},
                });
            }
        } catch (const mf::core::ApiException& api_exception) {
            send_json(ws_error_payload(api_exception.error()));
        } catch (const std::exception&) {
            send_json({
                {"type", "error"},
                {"code", "invalid_websocket_payload"},
                {"message", "WebSocket 消息格式不合法"},
                {"details", boost::json::object{}},
            });
        }

        read();
    }

    void handle_auth(const boost::json::object& payload) {
        const auto token = require_ws_string(payload, "token");
        const auto authenticated = token_service_->authenticate_bearer("Bearer " + token, false);

        if (!authenticated) {
            clear_identity();
            send_json({
                {"type", "auth_result"},
                {"ok", false},
            });
            return;
        }

        // 同一条连接重复 auth 时，不要无脑把在线计数再次加 1。
        // 如果切成了另一个账号，还要先清掉旧账号的在线和订阅状态。
        if (online_marked_ && session_ && session_->player_id != authenticated->player_id) {
            leave_channel();
            state_service_->mark_offline(session_->player_id);
            online_marked_ = false;
        }

        session_ = authenticated;
        if (!online_marked_) {
            // 鉴权成功意味着这个 websocket 会话已经拥有一个有效在线身份，
            // 所以这里把玩家标记为在线。
            state_service_->mark_online(session_->player_id);
            online_marked_ = true;
        }

        send_json({
            {"type", "auth_result"},
            {"ok", true},
            {"player_id", session_->player_id},
            {"nickname", session_->nickname},
        });
    }

    void clear_identity() {
        leave_channel();
        if (session_ && online_marked_) {
            state_service_->mark_offline(session_->player_id);
            online_marked_ = false;
        }
        session_.reset();
    }

    bool validate_session() {
        // HTTP 登出/刷新发生在另一进程，不能相信网关本地的旧 Session。
        if (session_ && token_service_->authenticate_bearer("Bearer " + session_->token, false)) {
            return true;
        }
        clear_identity();
        send_json({
            {"type", "error"},
            {"code", "unauthorized"},
            {"message", "请先完成有效鉴权"},
        });
        return false;
    }

    void handle_join(const boost::json::object& payload) {
        if (!session_) {
            send_json({
                {"type", "error"},
                {"message", "请先完成鉴权"},
            });
            return;
        }

        const auto channel = parse_channel(payload);
        if (!channel) {
            send_json({
                {"type", "error"},
                {"message", "频道参数不合法"},
            });
            return;
        }
        leave_channel();

        // 第一版里每个 websocket session 只持有一个房间订阅，
        // 这样状态最简单；以后要扩多房间，可以在这个基础上继续演进。
        active_channel_ = channel->channel_id;
        active_channel_spec_ = *channel;
        state_service_->join_channel(*channel, session_->player_id);
        state_service_->mark_channel_read(session_->player_id, channel->channel_id);
        const auto generation = ++subscription_generation_;
        subscription_id_ = chat_service_->subscribe(
            *channel,
            [weak = weak_from_this(), generation](const mf::game::ChatMessage& message) {
                if (const auto self = weak.lock()) {
                    // 广播来自其他连接/线程；身份和订阅只能在本连接的strand访问。
                    net::post(self->websocket_.get_executor(), [self, message, generation] {
                        if (self->closed_ || generation != self->subscription_generation_) {
                            return;
                        }
                        try {
                            if (self->validate_session()) {
                                self->send_json({
                                    {"type", "message"},
                                    {"message", mf::game::to_json(message)},
                                });
                            }
                        } catch (const std::exception&) {
                            // 存储不可用时不能沿用旧身份推送，也不能让异常逃出io_context。
                            self->close_transport();
                        }
                    });
                }
            });

        send_json({
            {"type", "join_result"},
            {"ok", true},
            {"channel", mf::game::to_json(*channel)},
        });
    }

    void handle_say(const boost::json::object& payload) {
        if (!session_ || active_channel_.empty()) {
            send_json({
                {"type", "error"},
                {"message", "请先加入频道"},
            });
            return;
        }

        const auto message_kind =
            payload.if_contains("message_kind")
                ? parse_message_kind(require_ws_string(payload, "message_kind"))
                : std::optional<mf::game::ChatMessageKind>{mf::game::ChatMessageKind::text};
        if (!message_kind) {
            throw_ws_error(
                "invalid_message_kind",
                "消息类型不合法",
                "message_kind");
        }

        // publish 一次同时做两件事：给当前订阅者扇出消息，
        // 并通过 ChatRepository 落存，这样后续拉历史时看到的是同一份数据。
        const auto result = chat_service_->publish(mf::game::ChatMessage{
            .channel = active_channel_spec_,
            .kind = *message_kind,
            .sender_player_id = session_->player_id,
            .sender_nickname = session_->nickname,
            .text = require_ws_string(payload, "text"),
            .sent_at = std::chrono::system_clock::now(),
        });
        if (!result.ok) {
            send_json({
                {"type", "error"},
                {"code", result.code},
                {"message", result.message},
            });
        }
    }

    void handle_recall(const boost::json::object& payload) {
        if (!session_ || active_channel_.empty()) {
            send_json({
                {"type", "error"},
                {"message", "请先加入频道"},
            });
            return;
        }

        const auto recalled = chat_service_->recall(
            active_channel_,
            require_ws_string(payload, "message_id"),
            session_->player_id);
        send_json({
            {"type", "recall_result"},
            {"ok", recalled.has_value()},
            {"message",
             recalled ? boost::json::value{mf::game::to_json(*recalled)} : boost::json::value{}},
        });
    }

    void handle_history(const boost::json::object& payload) {
        const auto channel = parse_channel(payload);
        if (!channel) {
            throw_ws_error("invalid_channel_kind", "频道类型不合法", "channel_kind");
        }
        const auto limit = read_optional_ws_size(payload, "limit", 20);
        send_json({
            {"type", "history_result"},
            {"channel", mf::game::to_json(*channel)},
            {"items", mf::game::to_json_array(chat_service_->history(channel->channel_id, limit))},
        });
    }

    void send_json(const boost::json::object& payload) {
        const std::string serialized = boost::json::serialize(payload);
        const auto message_size = serialized.size();
        const auto previous_count = queued_outbox_messages_.fetch_add(1, std::memory_order_relaxed);
        const auto previous_bytes = queued_outbox_bytes_.fetch_add(message_size, std::memory_order_relaxed);
        const bool message_limit_exceeded = previous_count >= options_.max_outbox_messages_per_connection;
        const bool byte_limit_exceeded =
            previous_bytes > options_.max_outbox_bytes_per_connection ||
            message_size > options_.max_outbox_bytes_per_connection -
                               std::min(previous_bytes, options_.max_outbox_bytes_per_connection);
        if (message_limit_exceeded || byte_limit_exceeded) {
            queued_outbox_messages_.fetch_sub(1, std::memory_order_relaxed);
            queued_outbox_bytes_.fetch_sub(message_size, std::memory_order_relaxed);
            net::post(websocket_.get_executor(), [self = shared_from_this()] {
                if (self->stats_ && self->stats_->metrics) {
                    self->stats_->metrics->increment_counter("mf_chat_gateway_slow_client_disconnects_total");
                }
                self->close_transport();
            });
            return;
        }

        net::post(
            websocket_.get_executor(),
            [self = shared_from_this(), serialized, message_size]() {
                if (self->closed_) {
                    self->release_outbox_reservation(message_size);
                    return;
                }
                // Beast 不允许同一个 websocket 流上并发写，
                // 所以这里通过 outbox 串行化所有出站消息帧。
                const bool writing = !self->outbox_.empty();
                self->outbox_.push_back(serialized);
                if (!writing) {
                    self->write();
                }
            });
    }

    void write() {
        websocket_.text(true);
        const auto write_started_at = std::chrono::steady_clock::now();
        websocket_.async_write(
            net::buffer(outbox_.front()),
            [self = shared_from_this(), write_started_at](boost::beast::error_code ec, std::size_t) {
                if (ec) {
                    self->close_transport();
                    return;
                }
                const auto written_size = self->outbox_.front().size();
                self->release_outbox_reservation(written_size);
                self->outbox_.pop_front();
                if (self->stats_ && self->stats_->metrics) {
                    self->stats_->metrics->observe_histogram_ms(
                        "mf_chat_gateway_write_duration_ms",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - write_started_at)
                            .count());
                }
                if (!self->outbox_.empty()) {
                    self->write();
                }
            });
    }

    void release_outbox_reservation(std::size_t message_size) {
        queued_outbox_bytes_.fetch_sub(message_size, std::memory_order_relaxed);
        queued_outbox_messages_.fetch_sub(1, std::memory_order_relaxed);
    }

    void close_transport() {
        if (closed_) {
            return;
        }
        closed_ = true;
        cleanup();
        for (const auto& message : outbox_) {
            release_outbox_reservation(message.size());
        }
        outbox_.clear();
        boost::system::error_code ec;
        auto& socket = websocket_.next_layer();
        socket.cancel(ec);
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    void leave_channel() {
        ++subscription_generation_;
        if (!active_channel_.empty() && subscription_id_ != 0 && session_) {
            chat_service_->unsubscribe(active_channel_, subscription_id_);
            state_service_->leave_channel(active_channel_, session_->player_id);
            active_channel_.clear();
            active_channel_spec_ = {};
            subscription_id_ = 0;
        }
    }

    std::optional<mf::game::ChatChannel> parse_channel(const boost::json::object& payload) const {
        if (!payload.if_contains("channel_kind")) {
            return std::nullopt;
        }

        const auto kind = parse_channel_kind(require_ws_string(payload, "channel_kind"));
        if (!kind) {
            return std::nullopt;
        }

        switch (*kind) {
        case mf::game::ChatChannelKind::lobby: {
            const auto scope = read_optional_ws_string(payload, "scope_id", "global");
            return make_lobby_channel(scope);
        }
        case mf::game::ChatChannelKind::room:
            if (!payload.if_contains("scope_id")) {
                throw_ws_error("missing_required_field", "缺少必填字段", "scope_id");
            }
            return make_room_channel(require_ws_string(payload, "scope_id"));
        case mf::game::ChatChannelKind::friend_direct:
            if (!payload.if_contains("friend_player_id") || !session_) {
                if (!session_) {
                    return std::nullopt;
                }
                throw_ws_error("missing_required_field", "缺少必填字段", "friend_player_id");
            }
            return make_friend_channel(
                session_->player_id,
                require_ws_string(payload, "friend_player_id"));
        }
        return std::nullopt;
    }

    void cleanup() {
        if (cleaned_up_) {
            return;
        }
        cleaned_up_ = true;
        // 这里必须显式退订，因为房间通常是长生命周期的，
        // 连接却不是；如果不清理，失效 session 仍会被继续广播。
        leave_channel();
        if (session_ && online_marked_) {
            state_service_->mark_offline(session_->player_id);
            online_marked_ = false;
        }
        if (counted_active_session_) {
            counted_active_session_ = false;
            const auto remaining = stats_->active_sessions.fetch_sub(1, std::memory_order_relaxed) - 1;
            set_gateway_active_sessions(stats_, remaining);
        }
    }

    websocket::stream<tcp::socket> websocket_;
    GatewayOptions options_;
    std::shared_ptr<GatewayRuntimeStats> stats_;
    std::shared_ptr<ChatService> chat_service_;
    std::shared_ptr<ChatStateService> state_service_;
    std::shared_ptr<mf::auth::TokenService> token_service_;
    std::shared_ptr<mf::core::Logger> logger_;
    boost::beast::flat_buffer buffer_;
    std::optional<mf::game::Session> session_;
    bool online_marked_ = false;
    std::string active_channel_;
    mf::game::ChatChannel active_channel_spec_;
    std::size_t subscription_id_ = 0;
    std::uint64_t subscription_generation_ = 0;
    std::deque<std::string> outbox_;
    std::atomic<std::size_t> queued_outbox_bytes_{0};
    std::atomic<std::size_t> queued_outbox_messages_{0};
    bool counted_active_session_ = true;
    bool cleaned_up_ = false;
    bool closed_ = false;
};

class ChatGateway::Listener : public std::enable_shared_from_this<ChatGateway::Listener> {
public:
    Listener(
        net::io_context& io_context,
        tcp::endpoint endpoint,
        GatewayOptions options,
        std::shared_ptr<GatewayRuntimeStats> stats,
        std::shared_ptr<ChatService> chat_service,
        std::shared_ptr<ChatStateService> state_service,
        std::shared_ptr<mf::auth::TokenService> token_service,
        std::shared_ptr<mf::core::Logger> logger)
        : acceptor_(net::make_strand(io_context)),
          socket_(net::make_strand(io_context)),
          options_(std::move(options)),
          stats_(std::move(stats)),
          chat_service_(std::move(chat_service)),
          state_service_(std::move(state_service)),
          token_service_(std::move(token_service)),
          logger_(std::move(logger)) {
        boost::beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("failed to open websocket acceptor");
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("failed to set websocket reuse_address");
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("failed to bind websocket endpoint");
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("failed to listen websocket endpoint");
        }
    }

    void run() {
        accept();
    }

private:
    void accept() {
        acceptor_.async_accept(
            socket_,
            [self = shared_from_this()](boost::beast::error_code ec) {
                if (!ec) {
                    const auto active = self->stats_->active_sessions.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (active > self->options_.max_connections) {
                        const auto remaining = self->stats_->active_sessions.fetch_sub(1, std::memory_order_relaxed) - 1;
                        set_gateway_active_sessions(self->stats_, remaining);
                        if (self->stats_->metrics) {
                            self->stats_->metrics->increment_counter(
                                "mf_chat_gateway_connections_total", 1.0, {{"result", "rejected_limit"}});
                        }
                        boost::system::error_code close_error;
                        self->socket_.close(close_error);
                        self->accept();
                        return;
                    }
                    set_gateway_active_sessions(self->stats_, active);
                    if (self->stats_->metrics) {
                        self->stats_->metrics->increment_counter(
                            "mf_chat_gateway_connections_total", 1.0, {{"result", "accepted"}});
                    }
                    // HTTP 和 WebSocket 故意拆成不同 listener，
                    // 这样聊天流量不会把业务 API 的复杂度一起抬高。
                    std::make_shared<GatewaySession>(
                        std::move(self->socket_),
                        self->options_,
                        self->stats_,
                        self->chat_service_,
                        self->state_service_,
                        self->token_service_,
                        self->logger_)
                        ->run();
                }
                self->accept();
            });
    }

    tcp::acceptor acceptor_;
    tcp::socket socket_;
    GatewayOptions options_;
    std::shared_ptr<GatewayRuntimeStats> stats_;
    std::shared_ptr<ChatService> chat_service_;
    std::shared_ptr<ChatStateService> state_service_;
    std::shared_ptr<mf::auth::TokenService> token_service_;
    std::shared_ptr<mf::core::Logger> logger_;
};

ChatGateway::ChatGateway(
    GatewayOptions options,
    std::shared_ptr<ChatService> chat_service,
    std::shared_ptr<ChatStateService> state_service,
    std::shared_ptr<mf::auth::TokenService> token_service,
    std::shared_ptr<mf::core::Logger> logger,
    std::shared_ptr<mf::core::MetricsRegistry> metrics)
    : options_(std::move(options)),
      chat_service_(std::move(chat_service)),
      state_service_(std::move(state_service)),
      token_service_(std::move(token_service)),
      logger_(std::move(logger)),
      metrics_(std::move(metrics)),
      io_context_(static_cast<int>(options_.thread_count)) {}

void ChatGateway::run() {
    const auto endpoint = tcp::endpoint{options_.host, options_.port};
    auto stats = std::make_shared<GatewayRuntimeStats>();
    stats->metrics = metrics_;
    if (metrics_) {
        metrics_->set_gauge("mf_chat_gateway_active_sessions", 0.0);
    }
    std::make_shared<Listener>(
        io_context_, endpoint, options_, stats, chat_service_, state_service_, token_service_, logger_)->run();

    if (options_.metrics_port != 0) {
        const auto metrics_endpoint = tcp::endpoint{options_.metrics_host, options_.metrics_port};
        std::make_shared<MetricsHttpListener>(io_context_, metrics_endpoint, metrics_)->run();
        logger_->log(
            mf::core::LogLevel::info,
            "chat metrics listening on " + metrics_endpoint.address().to_string() + ":" +
                std::to_string(metrics_endpoint.port()) + " path=/metrics");
    }

    logger_->log(
        mf::core::LogLevel::info,
        "chat gateway listening on " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));
    logger_->log(
        mf::core::LogLevel::info,
        "chat gateway limits max_connections=" + std::to_string(options_.max_connections) +
            " max_message_bytes=" + std::to_string(options_.max_message_bytes) +
            " max_outbox_bytes_per_connection=" + std::to_string(options_.max_outbox_bytes_per_connection) +
            " max_outbox_messages_per_connection=" + std::to_string(options_.max_outbox_messages_per_connection));

    std::vector<std::jthread> workers;
    workers.reserve(options_.thread_count);
    for (std::size_t i = 0; i < options_.thread_count; ++i) {
        workers.emplace_back([this] {
            io_context_.run();
        });
    }
}

}  // namespace mf::chat 命名空间
