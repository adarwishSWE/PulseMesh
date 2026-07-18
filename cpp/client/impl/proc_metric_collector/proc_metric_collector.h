#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "cpp/client/i_metric_collector.h"

namespace pulsemesh {

// Single-threaded collector for aggregate Linux CPU and memory metrics.
class ProcMetricCollector final : public IMetricCollector {
public:
    using SourceReader =
        std::function<std::expected<std::string, MetricCollectionError>(std::string_view source)>;

    ProcMetricCollector();
    ProcMetricCollector(SourceReader source_reader, std::string stat_source,
                        std::string meminfo_source);
    ~ProcMetricCollector() override = default;

    [[nodiscard]] std::expected<std::vector<Metric>, MetricCollectionError>
    collect(std::chrono::system_clock::time_point sampled_at) override;

    ProcMetricCollector(const ProcMetricCollector&) = delete;
    ProcMetricCollector& operator=(const ProcMetricCollector&) = delete;
    ProcMetricCollector(ProcMetricCollector&&) = delete;
    ProcMetricCollector& operator=(ProcMetricCollector&&) = delete;

private:
    struct CpuCounters {
        std::uint64_t total;
        std::uint64_t idle;
    };

    struct SourceSnapshot {
        std::string_view contents;
        std::string_view source;
    };

    [[nodiscard]] static std::expected<std::string, MetricCollectionError>
    read_source(std::string_view source);
    [[nodiscard]] static std::expected<CpuCounters, MetricCollectionError>
    parse_cpu_counters(SourceSnapshot snapshot);
    [[nodiscard]] static std::expected<std::vector<Metric>, MetricCollectionError>
    parse_memory_metrics(SourceSnapshot snapshot, std::int64_t timestamp_ms);

    SourceReader source_reader_;
    std::string stat_source_;
    std::string meminfo_source_;
    std::optional<CpuCounters> previous_cpu_counters_;
};

} // namespace pulsemesh
