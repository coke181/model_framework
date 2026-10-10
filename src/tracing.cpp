#include "mf/core/tracing.hpp"

// 文件职责：
// 1. 对外保留统一 tracing 外观层，避免业务代码直接依赖具体观测 SDK。
// 2. 当 MF_ENABLE_OTEL=1 时，内部接官方 opentelemetry-cpp + OTLP HTTP exporter。
// 3. 当 MF_ENABLE_OTEL=0 时，退回纯进程内 tracing，实现 Linux 最小依赖编译与 /debug/traces 调试。
//
// 调用流程：
// 1. make_tracer_provider(...) 创建 TracerProvider。
// 2. configure_global_tracer(...) 注册全局 provider，供 HTTP 中间件/任务/仓储复用。
// 3. start_span(...) 开启 span，并把当前上下文压入线程本地栈。
// 4. add_current_span_attribute(...) 给当前活动 span 补业务属性。
// 5. span 结束后写入 recent_spans 窗口，并按需输出日志或导出到 OTel collector。

#include "mf/core/logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <deque>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if MF_ENABLE_OTEL
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/common/attribute_utils.h>
#include <opentelemetry/sdk/common/exporter_utils.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/multi_span_processor.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/span_data.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/tracer.h>
#endif

namespace mf::core {

namespace {

std::mutex g_global_tracer_mutex;
std::shared_ptr<TracerProvider> g_global_tracer;

bool is_lower_hex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
    });
}

std::string bool_text(bool value) {
    return value ? "true" : "false";
}

std::string random_hex_string(std::size_t chars) {
    static std::mt19937_64 generator{std::random_device{}()};
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    std::uniform_int_distribution<unsigned long long> distribution;
    std::ostringstream output;
    while (output.tellp() < static_cast<std::streampos>(chars)) {
        output << std::hex << std::setw(16) << std::setfill('0') << distribution(generator);
    }
    auto value = output.str();
    value.resize(chars);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void log_span_snapshot(const std::shared_ptr<Logger>& logger, const std::string& service_name, const SpanSnapshot& snapshot) {
    if (!logger) {
        return;
    }

    std::ostringstream output;
    output << "trace_span service.name=" << service_name
           << " name=" << snapshot.name
           << " trace_id=" << snapshot.context.trace_id
           << " span_id=" << snapshot.context.span_id
           << " parent_span_id=" << snapshot.parent_span_id.value_or(std::string{})
           << " status=" << snapshot.status;
    for (const auto& attribute : snapshot.attributes) {
        output << " " << attribute.key << "=" << attribute.value;
    }
    logger->log(LogLevel::info, output.str());
}

SpanAttribute* find_attribute(std::vector<SpanAttribute>& attributes, const std::string& key) {
    auto it = std::find_if(attributes.begin(), attributes.end(), [&](const SpanAttribute& attribute) {
        return attribute.key == key;
    });
    return it == attributes.end() ? nullptr : &(*it);
}

}  // namespace

struct TracerProviderState {
    std::shared_ptr<Logger> logger;
    std::string service_name;
    mutable std::mutex mutex;
    std::deque<SpanSnapshot> recent_spans;
#if MF_ENABLE_OTEL
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;
#endif
};

namespace {

void append_recent_span(TracerProviderState& state, SpanSnapshot snapshot) {
    std::scoped_lock lock{state.mutex};
    state.recent_spans.push_back(std::move(snapshot));
    while (state.recent_spans.size() > 200) {
        state.recent_spans.pop_front();
    }
}

}  // namespace

#if MF_ENABLE_OTEL
namespace otel_api = opentelemetry::trace;
namespace otel_exporter_otlp = opentelemetry::exporter::otlp;
namespace otel_nostd = opentelemetry::nostd;
namespace otel_resource = opentelemetry::sdk::resource;
namespace otel_sdk_common = opentelemetry::sdk::common;
namespace otel_sdk_trace = opentelemetry::sdk::trace;

namespace {

template <std::size_t N>
std::string bytes_to_lower_hex(const std::array<std::uint8_t, N>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(N * 2);
    for (std::size_t i = 0; i < N; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0f];
    }
    return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> hex_to_bytes(std::string_view value) {
    if (value.size() != N * 2 || !is_lower_hex(value)) {
        return std::nullopt;
    }

    auto decode_nibble = [](char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') {
            return static_cast<std::uint8_t>(ch - '0');
        }
        return static_cast<std::uint8_t>(10 + (ch - 'a'));
    };

    std::array<std::uint8_t, N> bytes{};
    for (std::size_t i = 0; i < N; ++i) {
        bytes[i] = static_cast<std::uint8_t>(
            (decode_nibble(value[i * 2]) << 4) | decode_nibble(value[i * 2 + 1]));
    }
    return bytes;
}

std::string trace_id_to_string(const otel_api::TraceId& trace_id) {
    std::array<std::uint8_t, otel_api::TraceId::kSize> bytes{};
    trace_id.CopyBytesTo(otel_nostd::span<std::uint8_t, otel_api::TraceId::kSize>{bytes});
    return bytes_to_lower_hex(bytes);
}

std::string span_id_to_string(const otel_api::SpanId& span_id) {
    std::array<std::uint8_t, otel_api::SpanId::kSize> bytes{};
    span_id.CopyBytesTo(otel_nostd::span<std::uint8_t, otel_api::SpanId::kSize>{bytes});
    return bytes_to_lower_hex(bytes);
}

std::string trace_flags_to_string(const otel_api::TraceFlags& flags) {
    std::array<std::uint8_t, 1> bytes{};
    flags.CopyBytesTo(otel_nostd::span<std::uint8_t, 1>{bytes});
    return bytes_to_lower_hex(bytes);
}

std::optional<TraceContext> make_trace_context(const otel_api::SpanContext& context) {
    if (!context.IsValid()) {
        return std::nullopt;
    }
    return TraceContext{
        .trace_id = trace_id_to_string(context.trace_id()),
        .span_id = span_id_to_string(context.span_id()),
        .trace_flags = trace_flags_to_string(context.trace_flags()),
    };
}

std::optional<otel_api::SpanContext> make_span_context(const TraceContext& context, bool is_remote = true) {
    const auto trace_id_bytes = hex_to_bytes<otel_api::TraceId::kSize>(context.trace_id);
    const auto span_id_bytes = hex_to_bytes<otel_api::SpanId::kSize>(context.span_id);
    const auto flags_bytes = hex_to_bytes<1>(context.trace_flags);
    if (!trace_id_bytes || !span_id_bytes || !flags_bytes) {
        return std::nullopt;
    }

    return otel_api::SpanContext(
        otel_api::TraceId(otel_nostd::span<const std::uint8_t, otel_api::TraceId::kSize>{*trace_id_bytes}),
        otel_api::SpanId(otel_nostd::span<const std::uint8_t, otel_api::SpanId::kSize>{*span_id_bytes}),
        otel_api::TraceFlags((*flags_bytes)[0]),
        is_remote);
}

std::chrono::system_clock::time_point to_system_clock_time(opentelemetry::common::SystemTimestamp value) {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(value.time_since_epoch())};
}

std::string status_code_to_string(otel_api::StatusCode code) {
    switch (code) {
        case otel_api::StatusCode::kOk:
            return "ok";
        case otel_api::StatusCode::kError:
            return "error";
        case otel_api::StatusCode::kUnset:
        default:
            return "unset";
    }
}

struct OwnedAttributeValuePrinter {
    std::string operator()(bool value) const { return bool_text(value); }
    std::string operator()(std::int32_t value) const { return std::to_string(value); }
    std::string operator()(std::uint32_t value) const { return std::to_string(value); }
    std::string operator()(std::int64_t value) const { return std::to_string(value); }
    std::string operator()(std::uint64_t value) const { return std::to_string(value); }
    std::string operator()(double value) const {
        std::ostringstream output;
        output << value;
        return output.str();
    }
    std::string operator()(const std::string& value) const { return value; }
    std::string operator()(const std::vector<bool>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<std::int32_t>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<std::uint32_t>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<std::int64_t>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<double>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<std::string>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<std::uint64_t>& values) const { return join_vector(values); }
    std::string operator()(const std::vector<std::uint8_t>& values) const { return join_vector(values); }

    template <class T>
    std::string join_vector(const std::vector<T>& values) const {
        std::ostringstream output;
        output << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                output << ",";
            }
            output << values[i];
        }
        output << "]";
        return output.str();
    }
};

class DebugSpanExporter final : public otel_sdk_trace::SpanExporter {
public:
    explicit DebugSpanExporter(std::shared_ptr<TracerProviderState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<otel_sdk_trace::Recordable> MakeRecordable() noexcept override {
        return std::make_unique<otel_sdk_trace::SpanData>();
    }

    otel_sdk_common::ExportResult Export(
        const otel_nostd::span<std::unique_ptr<otel_sdk_trace::Recordable>>& spans) noexcept override {
        for (const auto& recordable : spans) {
            const auto* data = static_cast<const otel_sdk_trace::SpanData*>(recordable.get());
            if (!data) {
                continue;
            }
            const auto snapshot = to_snapshot(*data);
            append_recent_span(*state_, snapshot);
            log_span_snapshot(state_->logger, state_->service_name, snapshot);
        }
        return otel_sdk_common::ExportResult::kSuccess;
    }

    bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override {
        static_cast<void>(timeout);
        return true;
    }

    bool Shutdown(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override {
        static_cast<void>(timeout);
        return true;
    }

private:
    SpanSnapshot to_snapshot(const otel_sdk_trace::SpanData& span) const {
        std::vector<SpanAttribute> attributes;
        attributes.reserve(span.GetAttributes().size());
        for (const auto& [key, value] : span.GetAttributes()) {
            attributes.push_back(SpanAttribute{
                .key = key,
                .value = otel_nostd::visit(OwnedAttributeValuePrinter{}, value),
            });
        }

        const auto started_at = to_system_clock_time(span.GetStartTime());
        return SpanSnapshot{
            .name = std::string(span.GetName()),
            .context = make_trace_context(span.GetSpanContext()).value_or(TraceContext{}),
            .parent_span_id =
                span.GetParentSpanId().IsValid()
                    ? std::optional<std::string>{span_id_to_string(span.GetParentSpanId())}
                    : std::nullopt,
            .started_at = started_at,
            .ended_at = started_at + std::chrono::duration_cast<std::chrono::system_clock::duration>(span.GetDuration()),
            .attributes = std::move(attributes),
            .status = status_code_to_string(span.GetStatus()),
        };
    }

    std::shared_ptr<TracerProviderState> state_;
};

}  // namespace

struct SpanScopeImpl {
    otel_nostd::shared_ptr<otel_api::Span> span;
    std::unique_ptr<otel_api::Scope> scope;
};

SpanScope::SpanScope(std::shared_ptr<TracerProvider> provider, SpanSnapshot snapshot, bool push_current) {
    if (!provider || !provider->state_ || !provider->state_->tracer) {
        return;
    }

    otel_api::StartSpanOptions options;
    if (const auto parent = make_span_context(snapshot.context)) {
        options.parent = *parent;
    }

    const auto span = provider->state_->tracer->StartSpan(snapshot.name, options);
    if (!span || !span->GetContext().IsValid()) {
        return;
    }

    impl_ = std::make_shared<SpanScopeImpl>();
    impl_->span = span;
    if (push_current) {
        impl_->scope = std::make_unique<otel_api::Scope>(span);
    }
    for (const auto& attribute : snapshot.attributes) {
        impl_->span->SetAttribute(attribute.key, attribute.value);
    }
    if (snapshot.status == "ok") {
        impl_->span->SetStatus(otel_api::StatusCode::kOk);
    } else if (snapshot.status == "error") {
        impl_->span->SetStatus(otel_api::StatusCode::kError);
    }
    context_ = make_trace_context(span->GetContext()).value_or(TraceContext{});
}

#else
namespace {

struct ActiveSpan {
    std::shared_ptr<TracerProviderState> provider_state;
    SpanSnapshot snapshot;
};

thread_local std::vector<ActiveSpan> g_active_spans;

}  // namespace

struct SpanScopeImpl {
    std::size_t stack_index = 0;
    bool push_current = false;
};

SpanScope::SpanScope(std::shared_ptr<TracerProvider> provider, SpanSnapshot snapshot, bool push_current) {
    if (!provider || !provider->state_) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    std::optional<TraceContext> parent_context;
    if (snapshot.context.trace_id.empty()) {
        if (!g_active_spans.empty()) {
            parent_context = g_active_spans.back().snapshot.context;
        }
    } else {
        parent_context = snapshot.context;
    }

    snapshot.started_at = now;
    snapshot.ended_at = {};
    snapshot.parent_span_id = parent_context ? std::optional<std::string>{parent_context->span_id} : std::nullopt;
    snapshot.context = TraceContext{
        .trace_id = parent_context ? parent_context->trace_id : TracerProvider::random_hex(32),
        .span_id = TracerProvider::random_hex(16),
        .trace_flags = parent_context ? parent_context->trace_flags : "01",
    };

    context_ = snapshot.context;
    impl_ = std::make_shared<SpanScopeImpl>();
    impl_->stack_index = g_active_spans.size();
    impl_->push_current = push_current;
    g_active_spans.push_back(ActiveSpan{
        .provider_state = provider->state_,
        .snapshot = std::move(snapshot),
    });
}

#endif

SpanScope::SpanScope(SpanScope&& other) noexcept
    : context_(std::move(other.context_)),
      impl_(std::move(other.impl_)) {}

SpanScope& SpanScope::operator=(SpanScope&& other) noexcept {
    if (this != &other) {
        finish();
        context_ = std::move(other.context_);
        impl_ = std::move(other.impl_);
    }
    return *this;
}

SpanScope::~SpanScope() {
    finish();
}

void SpanScope::set_attribute(std::string key, std::string value) {
    if (!impl_) {
        return;
    }

#if MF_ENABLE_OTEL
    if (!impl_->span) {
        return;
    }
    impl_->span->SetAttribute(key, value);
#else
    if (impl_->stack_index >= g_active_spans.size()) {
        return;
    }
    auto& snapshot = g_active_spans[impl_->stack_index].snapshot;
    if (auto* attribute = find_attribute(snapshot.attributes, key)) {
        attribute->value = std::move(value);
    } else {
        snapshot.attributes.push_back(SpanAttribute{
            .key = std::move(key),
            .value = std::move(value),
        });
    }
#endif
}

void SpanScope::set_status(std::string status) {
    if (!impl_) {
        return;
    }

#if MF_ENABLE_OTEL
    if (!impl_->span) {
        return;
    }

    if (status == "ok") {
        impl_->span->SetStatus(otel_api::StatusCode::kOk);
    } else if (status == "error") {
        impl_->span->SetStatus(otel_api::StatusCode::kError);
    } else {
        impl_->span->SetStatus(otel_api::StatusCode::kUnset);
    }
#else
    if (impl_->stack_index >= g_active_spans.size()) {
        return;
    }
    g_active_spans[impl_->stack_index].snapshot.status = std::move(status);
#endif
}

const TraceContext& SpanScope::context() const {
    return context_;
}

bool SpanScope::valid() const {
#if MF_ENABLE_OTEL
    return impl_ && impl_->span && impl_->span->GetContext().IsValid();
#else
    return impl_ && impl_->stack_index < g_active_spans.size();
#endif
}

void SpanScope::finish() {
    if (!impl_) {
        return;
    }

#if MF_ENABLE_OTEL
    if (impl_->scope) {
        impl_->scope.reset();
    }
    if (impl_->span) {
        impl_->span->End();
        impl_->span = nullptr;
    }
#else
    if (impl_->stack_index < g_active_spans.size()) {
        auto active = std::move(g_active_spans[impl_->stack_index]);
        g_active_spans.erase(g_active_spans.begin() + static_cast<std::ptrdiff_t>(impl_->stack_index));
        active.snapshot.ended_at = std::chrono::system_clock::now();
        append_recent_span(*active.provider_state, active.snapshot);
        log_span_snapshot(active.provider_state->logger, active.provider_state->service_name, active.snapshot);
    }
#endif

    impl_ = nullptr;
}

TracerProvider::TracerProvider(std::shared_ptr<Logger> logger, std::string service_name)
    : state_(std::make_shared<TracerProviderState>()) {
    state_->logger = std::move(logger);
    state_->service_name = std::move(service_name);

#if MF_ENABLE_OTEL
    auto processors = std::vector<std::unique_ptr<otel_sdk_trace::SpanProcessor>>{};
    processors.push_back(otel_sdk_trace::SimpleSpanProcessorFactory::Create(
        std::make_unique<DebugSpanExporter>(state_)));

    otel_exporter_otlp::OtlpHttpExporterOptions otlp_options;
    otel_sdk_trace::BatchSpanProcessorOptions batch_options;
    processors.push_back(otel_sdk_trace::BatchSpanProcessorFactory::Create(
        otel_exporter_otlp::OtlpHttpExporterFactory::Create(otlp_options),
        batch_options));

    otel_resource::ResourceAttributes resource_attributes;
    resource_attributes.SetAttribute("service.name", state_->service_name);
    const auto resource = otel_resource::Resource::Create(resource_attributes);
    state_->provider = otel_nostd::shared_ptr<otel_api::TracerProvider>(
        new otel_sdk_trace::TracerProvider(std::move(processors), resource));
    state_->tracer = state_->provider->GetTracer("mf.core.tracing", "1.0.0");
#endif
}

SpanScope TracerProvider::start_span(
    std::string name,
    std::vector<SpanAttribute> attributes,
    std::optional<TraceContext> explicit_parent) {
    return SpanScope(
        shared_from_this(),
        SpanSnapshot{
            .name = std::move(name),
            .context = explicit_parent.value_or(TraceContext{}),
            .parent_span_id = std::nullopt,
            .started_at = std::chrono::system_clock::now(),
            .ended_at = {},
            .attributes = std::move(attributes),
            .status = "ok",
        },
        true);
}

SpanScope TracerProvider::start_span_with_trace(
    std::string name,
    std::string trace_id,
    std::optional<std::string> parent_span_id,
    std::vector<SpanAttribute> attributes) {
    return start_span(
        std::move(name),
        std::move(attributes),
        TraceContext{
            .trace_id = std::move(trace_id),
            .span_id = parent_span_id.value_or(random_hex(16)),
            .trace_flags = "01",
        });
}

std::optional<TraceContext> TracerProvider::current_context() const {
#if MF_ENABLE_OTEL
    const auto current_span = otel_api::Tracer::GetCurrentSpan();
    if (!current_span) {
        return std::nullopt;
    }
    return make_trace_context(current_span->GetContext());
#else
    if (g_active_spans.empty()) {
        return std::nullopt;
    }
    return g_active_spans.back().snapshot.context;
#endif
}

std::string TracerProvider::current_traceparent() const {
    const auto current = current_context();
    return current ? format_traceparent(*current) : std::string{};
}

std::vector<SpanSnapshot> TracerProvider::recent_spans(std::size_t limit) const {
    std::scoped_lock lock{state_->mutex};
    const auto count = std::min(limit, state_->recent_spans.size());
    return std::vector<SpanSnapshot>(
        state_->recent_spans.end() - static_cast<std::ptrdiff_t>(count),
        state_->recent_spans.end());
}

void TracerProvider::install_as_global_provider() const {
#if MF_ENABLE_OTEL
    if (state_ && state_->provider) {
        otel_api::Provider::SetTracerProvider(state_->provider);
    }
#endif
}

std::optional<TraceContext> TracerProvider::parse_traceparent(std::string_view value) {
    if (value.size() != 55 || value[2] != '-' || value[35] != '-' || value[52] != '-') {
        return std::nullopt;
    }
    const auto version = value.substr(0, 2);
    const auto trace_id = value.substr(3, 32);
    const auto span_id = value.substr(36, 16);
    const auto flags = value.substr(53, 2);
    if (version != "00" || !is_lower_hex(trace_id) || !is_lower_hex(span_id) || !is_lower_hex(flags)) {
        return std::nullopt;
    }
    return TraceContext{
        .trace_id = std::string{trace_id},
        .span_id = std::string{span_id},
        .trace_flags = std::string{flags},
    };
}

std::string TracerProvider::format_traceparent(const TraceContext& context) {
    return "00-" + context.trace_id + "-" + context.span_id + "-" + context.trace_flags;
}

std::string TracerProvider::random_hex(std::size_t chars) {
    return random_hex_string(chars);
}

std::shared_ptr<TracerProvider> make_tracer_provider(std::shared_ptr<Logger> logger, std::string service_name) {
    return std::make_shared<TracerProvider>(std::move(logger), std::move(service_name));
}

void configure_global_tracer(std::shared_ptr<TracerProvider> tracer) {
    std::scoped_lock lock{g_global_tracer_mutex};
    g_global_tracer = std::move(tracer);
    if (g_global_tracer) {
        g_global_tracer->install_as_global_provider();
    }
}

std::shared_ptr<TracerProvider> global_tracer() {
    std::scoped_lock lock{g_global_tracer_mutex};
    return g_global_tracer;
}

SpanScope start_current_span(
    std::string name,
    std::vector<SpanAttribute> attributes,
    std::optional<TraceContext> explicit_parent) {
    const auto tracer = global_tracer();
    if (!tracer) {
        return {};
    }
    return tracer->start_span(std::move(name), std::move(attributes), std::move(explicit_parent));
}

void add_current_span_attribute(std::string key, std::string value) {
#if MF_ENABLE_OTEL
    const auto current_span = otel_api::Tracer::GetCurrentSpan();
    if (!current_span || !current_span->GetContext().IsValid()) {
        return;
    }
    current_span->SetAttribute(key, value);
#else
    if (g_active_spans.empty()) {
        return;
    }
    auto& snapshot = g_active_spans.back().snapshot;
    if (auto* attribute = find_attribute(snapshot.attributes, key)) {
        attribute->value = std::move(value);
    } else {
        snapshot.attributes.push_back(SpanAttribute{
            .key = std::move(key),
            .value = std::move(value),
        });
    }
#endif
}

}  // namespace mf::core
