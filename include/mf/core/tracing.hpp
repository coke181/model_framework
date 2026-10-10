#pragma once

// 文件职责：
// 提供轻量级 tracing 抽象，先把 HTTP、后台任务、MySQL 操作串到同一条 trace 上。
// 调用流程：
// main() 创建并注册全局 TracerProvider ->
// HTTP 中间件抽取/创建 traceparent 并开启根 span ->
// 路由、任务 worker、仓储层按需开启子 span ->
// 结束后的 span 会写入最近窗口并输出结构化日志，便于接 OpenTelemetry 风格观测链路。

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mf::core {

class Logger;
struct SpanScopeImpl;
struct TracerProviderState;

struct TraceContext {
    std::string trace_id;
    std::string span_id;
    std::string trace_flags = "01";
};

struct SpanAttribute {
    std::string key;
    std::string value;
};

struct SpanSnapshot {
    std::string name;
    TraceContext context;
    std::optional<std::string> parent_span_id;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point ended_at;
    std::vector<SpanAttribute> attributes;
    std::string status = "unset";
};

class TracerProvider;

class SpanScope {
public:
    SpanScope() = default;
    SpanScope(std::shared_ptr<TracerProvider> provider, SpanSnapshot snapshot, bool push_current);
    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;
    SpanScope(SpanScope&& other) noexcept;
    SpanScope& operator=(SpanScope&& other) noexcept;
    ~SpanScope();

    void set_attribute(std::string key, std::string value);
    void set_status(std::string status);
    const TraceContext& context() const;
    bool valid() const;

private:
    void finish();

    TraceContext context_;
    std::shared_ptr<SpanScopeImpl> impl_;
};

class TracerProvider : public std::enable_shared_from_this<TracerProvider> {
public:
    explicit TracerProvider(std::shared_ptr<Logger> logger, std::string service_name);

    SpanScope start_span(
        std::string name,
        std::vector<SpanAttribute> attributes = {},
        std::optional<TraceContext> explicit_parent = std::nullopt);
    SpanScope start_span_with_trace(
        std::string name,
        std::string trace_id,
        std::optional<std::string> parent_span_id = std::nullopt,
        std::vector<SpanAttribute> attributes = {});

    std::optional<TraceContext> current_context() const;
    std::string current_traceparent() const;
    std::vector<SpanSnapshot> recent_spans(std::size_t limit) const;
    void install_as_global_provider() const;

    static std::optional<TraceContext> parse_traceparent(std::string_view value);
    static std::string format_traceparent(const TraceContext& context);

private:
    friend class SpanScope;

    static std::string random_hex(std::size_t chars);

    std::shared_ptr<TracerProviderState> state_;
};

std::shared_ptr<TracerProvider> make_tracer_provider(std::shared_ptr<Logger> logger, std::string service_name);
void configure_global_tracer(std::shared_ptr<TracerProvider> tracer);
std::shared_ptr<TracerProvider> global_tracer();
SpanScope start_current_span(
    std::string name,
    std::vector<SpanAttribute> attributes = {},
    std::optional<TraceContext> explicit_parent = std::nullopt);
void add_current_span_attribute(std::string key, std::string value);

}  // namespace mf::core
