#pragma once

// 文件职责：
// 定义一个轻量级进程内指标注册表，先服务 hello 示例和框架基础观测。
// 调用流程：
// main()/scheduler/仓储等组件持有 MetricsRegistry ->
// 在请求、任务、存储操作处写入 counter/histogram ->
// /metrics 路由把当前进程快照按 Prometheus 文本格式导出。

#include <memory>
#include <mutex>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mf::core {

using MetricLabels = std::vector<std::pair<std::string, std::string>>;
struct MetricsState;

class MetricsRegistry {
public:
    void increment_counter(std::string name, double value = 1.0, MetricLabels labels = {});
    void observe_histogram_ms(std::string name, double value_ms, MetricLabels labels = {});
    void set_gauge(std::string name, double value, MetricLabels labels = {});

    // 输出 Prometheus exposition text，方便直接被本地 Prometheus/agent 抓取。
    std::string render_prometheus() const;

private:
    friend std::shared_ptr<MetricsRegistry> make_metrics_registry(std::string service_name);

    struct CounterEntry {
        std::string name;
        MetricLabels labels;
        double value = 0.0;
    };

    struct HistogramEntry {
        std::string name;
        MetricLabels labels;
        std::vector<double> bucket_bounds;
        std::vector<std::uint64_t> bucket_counts;
        double sum = 0.0;
        std::uint64_t count = 0;
    };

    struct GaugeEntry {
        std::string name;
        MetricLabels labels;
        double value = 0.0;
    };

    static std::vector<double> default_histogram_buckets_ms();

    std::vector<CounterEntry>::iterator find_counter(
        const std::string& name,
        const MetricLabels& labels);
    std::vector<CounterEntry>::const_iterator find_counter(
        const std::string& name,
        const MetricLabels& labels) const;
    std::vector<HistogramEntry>::iterator find_histogram(
        const std::string& name,
        const MetricLabels& labels);
    std::vector<HistogramEntry>::const_iterator find_histogram(
        const std::string& name,
        const MetricLabels& labels) const;
    std::vector<GaugeEntry>::iterator find_gauge(
        const std::string& name,
        const MetricLabels& labels);
    std::vector<GaugeEntry>::const_iterator find_gauge(
        const std::string& name,
        const MetricLabels& labels) const;

    mutable std::mutex mutex_;
    std::vector<CounterEntry> counters_;
    std::vector<HistogramEntry> histograms_;
    std::vector<GaugeEntry> gauges_;
    std::shared_ptr<MetricsState> state_;
};

std::shared_ptr<MetricsRegistry> make_metrics_registry(std::string service_name = "model-framework");

}  // namespace mf::core
