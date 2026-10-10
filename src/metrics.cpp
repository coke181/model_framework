#include "mf/core/metrics.hpp"

// 文件职责：
// 1. 对外保留统一 MetricsRegistry，避免业务层直接依赖具体 metrics SDK。
// 2. 当 MF_ENABLE_OTEL=1 时，同时写入进程内快照和官方 OTLP metrics exporter。
// 3. 当 MF_ENABLE_OTEL=0 时，仅保留进程内聚合与 `/metrics` 文本导出，方便 Linux 最小依赖运行。
//
// 调用流程：
// 1. make_metrics_registry(service_name) 创建指标注册表。
// 2. 业务通过 increment_counter / observe_histogram_ms 写入指标。
// 3. `/metrics` 路由读取进程内快照并渲染 Prometheus exposition text。

#if MF_ENABLE_OTEL
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/view/view_registry.h>
#include <opentelemetry/sdk/resource/resource.h>
#endif

#include <algorithm>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace mf::core {

namespace {

bool labels_equal(const MetricLabels& left, const MetricLabels& right) {
    return left == right;
}

std::string escape_label_value(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

std::string format_labels(const MetricLabels& labels) {
    if (labels.empty()) {
        return {};
    }
    std::ostringstream output;
    output << '{';
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << labels[index].first << "=\"" << escape_label_value(labels[index].second) << '"';
    }
    output << '}';
    return output.str();
}

}  // namespace

#if MF_ENABLE_OTEL
namespace otel_exporter_otlp = opentelemetry::exporter::otlp;
namespace otel_metrics = opentelemetry::metrics;
namespace otel_nostd = opentelemetry::nostd;
namespace otel_sdk_metrics = opentelemetry::sdk::metrics;
namespace otel_resource = opentelemetry::sdk::resource;

namespace {

class MetricLabelIterable final : public opentelemetry::common::KeyValueIterable {
public:
    explicit MetricLabelIterable(const MetricLabels& labels)
        : labels_(labels) {}

    bool ForEachKeyValue(opentelemetry::nostd::function_ref<bool(opentelemetry::nostd::string_view, opentelemetry::common::AttributeValue)> callback) const noexcept override {
        for (const auto& [key, value] : labels_) {
            if (!callback(key, value)) {
                return false;
            }
        }
        return true;
    }

    std::size_t size() const noexcept override {
        return labels_.size();
    }

private:
    const MetricLabels& labels_;
};

using CounterInstrument = otel_nostd::unique_ptr<otel_metrics::Counter<double>>;
using HistogramInstrument = otel_nostd::unique_ptr<otel_metrics::Histogram<double>>;
using UpDownCounterInstrument = otel_nostd::unique_ptr<otel_metrics::UpDownCounter<double>>;

}  // namespace
#endif

struct MetricsState {
#if MF_ENABLE_OTEL
    explicit MetricsState(std::string service_name) {
        otel_resource::ResourceAttributes resource_attributes;
        resource_attributes.SetAttribute("service.name", service_name);
        const auto resource = otel_resource::Resource::Create(resource_attributes);

        auto provider = std::make_unique<otel_sdk_metrics::MeterProvider>(
            std::unique_ptr<otel_sdk_metrics::ViewRegistry>(new otel_sdk_metrics::ViewRegistry()),
            resource);

        otel_sdk_metrics::PeriodicExportingMetricReaderOptions reader_options;
        reader_options.export_interval_millis = std::chrono::milliseconds(5000);
        reader_options.export_timeout_millis = std::chrono::milliseconds(3000);
        provider->AddMetricReader(std::shared_ptr<otel_sdk_metrics::MetricReader>(
            otel_sdk_metrics::PeriodicExportingMetricReaderFactory::Create(
                otel_exporter_otlp::OtlpHttpMetricExporterFactory::Create(
                    otel_exporter_otlp::OtlpHttpMetricExporterOptions{}),
                reader_options)
                .release()));

        meter_provider = otel_nostd::shared_ptr<otel_metrics::MeterProvider>(provider.release());
        meter = meter_provider->GetMeter("mf.core.metrics", "1.0.0");
        otel_metrics::Provider::SetMeterProvider(meter_provider);
    }

    CounterInstrument& counter(const std::string& name, const std::string& unit) {
        auto& instrument = counters[name];
        if (!instrument && meter) {
            instrument = meter->CreateDoubleCounter(name, "", unit);
        }
        return instrument;
    }

    HistogramInstrument& histogram(const std::string& name, const std::string& unit) {
        auto& instrument = histograms[name];
        if (!instrument && meter) {
            instrument = meter->CreateDoubleHistogram(name, "", unit);
        }
        return instrument;
    }

    UpDownCounterInstrument& up_down_counter(const std::string& name, const std::string& unit) {
        auto& instrument = up_down_counters[name];
        if (!instrument && meter) {
            instrument = meter->CreateDoubleUpDownCounter(name, "", unit);
        }
        return instrument;
    }

    std::mutex mutex;
    otel_nostd::shared_ptr<otel_metrics::MeterProvider> meter_provider;
    otel_nostd::shared_ptr<otel_metrics::Meter> meter;
    std::unordered_map<std::string, CounterInstrument> counters;
    std::unordered_map<std::string, HistogramInstrument> histograms;
    std::unordered_map<std::string, UpDownCounterInstrument> up_down_counters;
#else
    explicit MetricsState(std::string service_name)
        : service_name(std::move(service_name)) {}

    std::string service_name;
#endif
};

void MetricsRegistry::increment_counter(std::string name, double value, MetricLabels labels) {
    {
        std::scoped_lock lock{mutex_};
        auto entry = find_counter(name, labels);
        if (entry == counters_.end()) {
            counters_.push_back(CounterEntry{
                .name = name,
                .labels = labels,
                .value = value,
            });
        } else {
            entry->value += value;
        }
    }

#if MF_ENABLE_OTEL
    if (!state_) {
        return;
    }

    std::scoped_lock lock{state_->mutex};
    auto& instrument = state_->counter(name, "1");
    if (instrument) {
        const MetricLabelIterable label_view{labels};
        instrument->Add(
            value,
            static_cast<const opentelemetry::common::KeyValueIterable&>(label_view),
            opentelemetry::context::Context{});
    }
#else
    static_cast<void>(name);
    static_cast<void>(value);
    static_cast<void>(labels);
#endif
}

void MetricsRegistry::observe_histogram_ms(std::string name, double value_ms, MetricLabels labels) {
    {
        std::scoped_lock lock{mutex_};
        auto entry = find_histogram(name, labels);
        if (entry == histograms_.end()) {
            histograms_.push_back(HistogramEntry{
                .name = name,
                .labels = labels,
                .bucket_bounds = default_histogram_buckets_ms(),
                .bucket_counts = std::vector<std::uint64_t>(default_histogram_buckets_ms().size() + 1, 0),
            });
            entry = std::prev(histograms_.end());
        }
        ++entry->count;
        entry->sum += value_ms;
        const auto bucket_index = std::lower_bound(entry->bucket_bounds.begin(), entry->bucket_bounds.end(), value_ms);
        const auto distance = static_cast<std::size_t>(std::distance(entry->bucket_bounds.begin(), bucket_index));
        ++entry->bucket_counts[distance];
    }

#if MF_ENABLE_OTEL
    if (!state_) {
        return;
    }

    std::scoped_lock lock{state_->mutex};
    auto& instrument = state_->histogram(name, "ms");
    if (instrument) {
        const MetricLabelIterable label_view{labels};
        instrument->Record(
            value_ms,
            static_cast<const opentelemetry::common::KeyValueIterable&>(label_view),
            opentelemetry::context::Context{});
    }
#else
    static_cast<void>(name);
    static_cast<void>(value_ms);
    static_cast<void>(labels);
#endif
}

void MetricsRegistry::set_gauge(std::string name, double value, MetricLabels labels) {
    double delta = value;
    {
        std::scoped_lock lock{mutex_};
        auto entry = find_gauge(name, labels);
        if (entry == gauges_.end()) {
            gauges_.push_back(GaugeEntry{
                .name = name,
                .labels = labels,
                .value = value,
            });
        } else {
            delta = value - entry->value;
            entry->value = value;
        }
    }

#if MF_ENABLE_OTEL
    if (!state_) {
        return;
    }

    std::scoped_lock lock{state_->mutex};
    auto& instrument = state_->up_down_counter(name, "1");
    if (instrument && delta != 0.0) {
        const MetricLabelIterable label_view{labels};
        instrument->Add(
            delta,
            static_cast<const opentelemetry::common::KeyValueIterable&>(label_view),
            opentelemetry::context::Context{});
    }
#else
    static_cast<void>(name);
    static_cast<void>(value);
    static_cast<void>(labels);
    static_cast<void>(delta);
#endif
}

std::string MetricsRegistry::render_prometheus() const {
    std::scoped_lock lock{mutex_};
    std::ostringstream output;
    for (const auto& counter : counters_) {
        output << "# TYPE " << counter.name << " counter\n";
        output << counter.name << format_labels(counter.labels) << ' ' << counter.value << '\n';
    }
    for (const auto& gauge : gauges_) {
        output << "# TYPE " << gauge.name << " gauge\n";
        output << gauge.name << format_labels(gauge.labels) << ' ' << gauge.value << '\n';
    }
    for (const auto& histogram : histograms_) {
        output << "# TYPE " << histogram.name << " histogram\n";
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < histogram.bucket_bounds.size(); ++index) {
            cumulative += histogram.bucket_counts[index];
            auto bucket_labels = histogram.labels;
            bucket_labels.emplace_back("le", std::to_string(histogram.bucket_bounds[index]));
            output << histogram.name << "_bucket" << format_labels(bucket_labels) << ' ' << cumulative << '\n';
        }
        cumulative += histogram.bucket_counts.back();
        auto inf_labels = histogram.labels;
        inf_labels.emplace_back("le", "+Inf");
        output << histogram.name << "_bucket" << format_labels(inf_labels) << ' ' << cumulative << '\n';
        output << histogram.name << "_sum" << format_labels(histogram.labels) << ' ' << histogram.sum << '\n';
        output << histogram.name << "_count" << format_labels(histogram.labels) << ' ' << histogram.count << '\n';
    }
    return output.str();
}

std::vector<double> MetricsRegistry::default_histogram_buckets_ms() {
    return {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
}

std::vector<MetricsRegistry::CounterEntry>::iterator MetricsRegistry::find_counter(
    const std::string& name,
    const MetricLabels& labels) {
    return std::find_if(
        counters_.begin(),
        counters_.end(),
        [&](const CounterEntry& entry) { return entry.name == name && labels_equal(entry.labels, labels); });
}

std::vector<MetricsRegistry::CounterEntry>::const_iterator MetricsRegistry::find_counter(
    const std::string& name,
    const MetricLabels& labels) const {
    return std::find_if(
        counters_.begin(),
        counters_.end(),
        [&](const CounterEntry& entry) { return entry.name == name && labels_equal(entry.labels, labels); });
}

std::vector<MetricsRegistry::HistogramEntry>::iterator MetricsRegistry::find_histogram(
    const std::string& name,
    const MetricLabels& labels) {
    return std::find_if(
        histograms_.begin(),
        histograms_.end(),
        [&](const HistogramEntry& entry) { return entry.name == name && labels_equal(entry.labels, labels); });
}

std::vector<MetricsRegistry::HistogramEntry>::const_iterator MetricsRegistry::find_histogram(
    const std::string& name,
    const MetricLabels& labels) const {
    return std::find_if(
        histograms_.begin(),
        histograms_.end(),
        [&](const HistogramEntry& entry) { return entry.name == name && labels_equal(entry.labels, labels); });
}

std::vector<MetricsRegistry::GaugeEntry>::iterator MetricsRegistry::find_gauge(
    const std::string& name,
    const MetricLabels& labels) {
    return std::find_if(
        gauges_.begin(),
        gauges_.end(),
        [&](const GaugeEntry& entry) { return entry.name == name && labels_equal(entry.labels, labels); });
}

std::vector<MetricsRegistry::GaugeEntry>::const_iterator MetricsRegistry::find_gauge(
    const std::string& name,
    const MetricLabels& labels) const {
    return std::find_if(
        gauges_.begin(),
        gauges_.end(),
        [&](const GaugeEntry& entry) { return entry.name == name && labels_equal(entry.labels, labels); });
}

std::shared_ptr<MetricsRegistry> make_metrics_registry(std::string service_name) {
    auto registry = std::make_shared<MetricsRegistry>();
    registry->state_ = std::make_shared<MetricsState>(std::move(service_name));
    return registry;
}

}  // namespace mf::core
