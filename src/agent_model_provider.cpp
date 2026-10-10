#include "mf/agent/model_provider.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <openssl/ssl.h>

#include <stdexcept>

// 文件职责：通过后端 HTTPS 调用 OpenAI-compatible Chat Completions。
// 安全边界：不记录 API Key、完整 Prompt、玩家 Access Token 或完整模型响应。

namespace mf::agent {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct ParsedEndpoint {
    bool tls = true;
    std::string host;
    std::string port;
    std::string target;
};

ParsedEndpoint parse_endpoint(const std::string& endpoint) {
    ParsedEndpoint parsed;
    std::size_t offset = 0;
    if (endpoint.starts_with("https://")) {
        parsed.tls = true;
        parsed.port = "443";
        offset = 8;
    } else if (endpoint.starts_with("http://")) {
        parsed.tls = false;
        parsed.port = "80";
        offset = 7;
    } else {
        throw std::runtime_error("Agent API Endpoint 必须使用 http:// 或 https://");
    }
    const auto slash = endpoint.find('/', offset);
    const auto authority = endpoint.substr(offset, slash == std::string::npos ? std::string::npos : slash - offset);
    parsed.target = slash == std::string::npos ? "/" : endpoint.substr(slash);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty()) throw std::runtime_error("Agent API Endpoint 缺少主机名");
    return parsed;
}

boost::json::object message_json(std::string role, std::string content) {
    return {{"role", std::move(role)}, {"content", std::move(content)}};
}

std::string build_request_body(const AgentOptions& options, const AgentModelRequest& request) {
    boost::json::array messages;
    messages.push_back(message_json("system", request.system_prompt));
    if (!request.authoritative_context.empty()) {
        messages.push_back(message_json("system", "后端权威游戏上下文：\n" + request.authoritative_context));
    }
    if (!request.client_context.empty()) {
        messages.push_back(message_json(
            "system",
            "以下是客户端临时上下文，只能用于建议，不能作为资产或任务完成依据：\n" + request.client_context));
    }
    for (const auto& item : request.history) {
        if (item.role == "user" || item.role == "assistant") {
            messages.push_back(message_json(item.role, item.content));
        }
    }
    messages.push_back(message_json("user", request.user_message));
    return boost::json::serialize(boost::json::object{
        {"model", options.model},
        {"stream", false},
        {"temperature", 0.3},
        {"messages", std::move(messages)},
    });
}

AgentModelResponse parse_model_response(const std::string& body) {
    const auto root = boost::json::parse(body).as_object();
    AgentModelResponse response;
    if (const auto* choices_value = root.if_contains("choices"); choices_value && choices_value->is_array()) {
        const auto& choices = choices_value->as_array();
        if (!choices.empty() && choices.front().is_object()) {
            const auto& choice = choices.front().as_object();
            if (const auto* message = choice.if_contains("message"); message && message->is_object()) {
                if (const auto* content = message->as_object().if_contains("content"); content && content->is_string()) {
                    response.content = content->as_string().c_str();
                }
            }
        }
    }
    if (const auto* usage = root.if_contains("usage"); usage && usage->is_object()) {
        const auto& object = usage->as_object();
        if (const auto* value = object.if_contains("prompt_tokens"); value && value->is_int64()) response.prompt_tokens = value->as_int64();
        if (const auto* value = object.if_contains("completion_tokens"); value && value->is_int64()) response.completion_tokens = value->as_int64();
    }
    response.success = !response.content.empty();
    if (!response.success) response.error_code = "agent_provider_invalid_response";
    return response;
}

class DisabledAgentModelProvider final : public AgentModelProvider {
public:
    AgentModelResponse complete(const AgentModelRequest&) const override {
        return {.error_code = "agent_provider_disabled"};
    }
};

class OpenAICompatibleModelProvider final : public AgentModelProvider {
public:
    explicit OpenAICompatibleModelProvider(AgentOptions options)
        : options_(std::move(options)), endpoint_(parse_endpoint(options_.api_endpoint)) {}

    AgentModelResponse complete(const AgentModelRequest& request) const override {
        try {
            asio::io_context context;
            tcp::resolver resolver{context};
            const auto endpoints = resolver.resolve(endpoint_.host, endpoint_.port);
            const auto body = build_request_body(options_, request);
            http::request<http::string_body> http_request{http::verb::post, endpoint_.target, 11};
            http_request.set(http::field::host, endpoint_.host);
            http_request.set(http::field::user_agent, "mf-game-agent/1.0");
            http_request.set(http::field::content_type, "application/json");
            http_request.set(http::field::authorization, "Bearer " + options_.api_key);
            http_request.body() = body;
            http_request.prepare_payload();

            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            parser.body_limit(2 * 1024 * 1024);
            if (endpoint_.tls) {
                ssl::context ssl_context{ssl::context::tls_client};
                ssl_context.set_default_verify_paths();
                ssl::stream<beast::tcp_stream> stream{context, ssl_context};
                if (!SSL_set_tlsext_host_name(stream.native_handle(), endpoint_.host.c_str())) {
                    throw std::runtime_error("Agent TLS SNI 配置失败");
                }
                stream.set_verify_mode(ssl::verify_peer);
                stream.set_verify_callback(ssl::host_name_verification(endpoint_.host));
                beast::get_lowest_layer(stream).expires_after(options_.timeout);
                beast::get_lowest_layer(stream).connect(endpoints);
                stream.handshake(ssl::stream_base::client);
                http::write(stream, http_request);
                http::read(stream, buffer, parser);
                beast::error_code ignored;
                stream.shutdown(ignored);
            } else {
                beast::tcp_stream stream{context};
                stream.expires_after(options_.timeout);
                stream.connect(endpoints);
                http::write(stream, http_request);
                http::read(stream, buffer, parser);
            }
            const auto response = parser.release();
            if (response.result_int() < 200 || response.result_int() >= 300) {
                return {.error_code = "agent_provider_http_" + std::to_string(response.result_int())};
            }
            return parse_model_response(response.body());
        } catch (...) {
            return {.error_code = "agent_provider_failed"};
        }
    }

private:
    AgentOptions options_;
    ParsedEndpoint endpoint_;
};

}  // namespace

std::shared_ptr<AgentModelProvider> make_agent_model_provider(const AgentOptions& options) {
    if (!options.enabled || options.api_endpoint.empty() || options.api_key.empty()) {
        return std::make_shared<DisabledAgentModelProvider>();
    }
    return std::make_shared<OpenAICompatibleModelProvider>(options);
}

}  // namespace mf::agent
