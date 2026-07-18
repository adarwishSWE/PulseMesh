#include "cpp/client/impl/proc_metric_collector/proc_metric_collector.h"

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <utility>

namespace pulsemesh {
namespace {

constexpr std::string_view kDefaultStatSource = "/proc/stat";
constexpr std::string_view kDefaultMeminfoSource = "/proc/meminfo";
constexpr std::uint64_t kBytesPerKilobyte = 1024;

struct MetricSample {
    std::string_view name_;
    double value_;
    std::int64_t timestamp_ms_;
};

Metric make_metric(MetricSample sample) {
    // Translate the collector's internal sample representation into the shared wire contract.
    Metric metric;
    metric.set_name(sample.name_);
    metric.set_value(sample.value_);
    metric.set_timestamp_ms(sample.timestamp_ms_);
    return metric;
}

MetricCollectionError make_error(MetricCollectionErrorCode code, std::string operation,
                                 std::string source, bool retryable) {
    return MetricCollectionError{
        .code = code,
        .operation = std::move(operation),
        .source = std::move(source),
        .retryable = retryable,
    };
}

} // namespace

ProcMetricCollector::ProcMetricCollector()
    : ProcMetricCollector(read_source, std::string{kDefaultStatSource},
                          std::string{kDefaultMeminfoSource}) {}

ProcMetricCollector::ProcMetricCollector(SourceReader source_reader, std::string stat_source,
                                         std::string meminfo_source)
    : source_reader_(std::move(source_reader)), stat_source_(std::move(stat_source)),
      meminfo_source_(std::move(meminfo_source)) {}

std::expected<std::vector<Metric>, MetricCollectionError>
ProcMetricCollector::collect(std::chrono::system_clock::time_point sampled_at) {
    // Read both Linux sources before mutating the previous CPU snapshot.
    const auto stat_contents = source_reader_(stat_source_);
    if (!stat_contents) {
        return std::unexpected(stat_contents.error());
    }

    const auto meminfo_contents = source_reader_(meminfo_source_);
    if (!meminfo_contents) {
        return std::unexpected(meminfo_contents.error());
    }

    // Parse the cumulative CPU counters and immediately surface malformed source data.
    const auto current_cpu =
        parse_cpu_counters(SourceSnapshot{.contents = *stat_contents, .source = stat_source_});
    if (!current_cpu) {
        return std::unexpected(current_cpu.error());
    }

    // Convert the caller's sampling time once and use it for every metric in this snapshot.
    const auto timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(sampled_at.time_since_epoch())
            .count();
    auto metrics = parse_memory_metrics(
        SourceSnapshot{.contents = *meminfo_contents, .source = meminfo_source_},
        timestamp_ms);
    if (!metrics) {
        return std::unexpected(metrics.error());
    }

    // The first cumulative CPU sample is only a baseline; memory metrics are already absolute.
    if (!previous_cpu_counters_) {
        previous_cpu_counters_ = *current_cpu;
        return metrics;
    }

    // Advance the stored baseline and reject reset or otherwise non-monotonic counters.
    const CpuCounters previous_cpu = *previous_cpu_counters_;
    previous_cpu_counters_ = *current_cpu;
    if (current_cpu->total <= previous_cpu.total || current_cpu->idle < previous_cpu.idle) {
        return std::unexpected(make_error(MetricCollectionErrorCode::kInvalidCounterDelta,
                                          "calculate_cpu_usage",
                                          stat_source_,
                                          true));
    }

    // Derive CPU utilization from active and total counter deltas, not absolute uptime counters.
    const std::uint64_t total_delta = current_cpu->total - previous_cpu.total;
    const std::uint64_t idle_delta = current_cpu->idle - previous_cpu.idle;
    if (idle_delta > total_delta) {
        return std::unexpected(make_error(MetricCollectionErrorCode::kInvalidCounterDelta,
                                          "calculate_cpu_usage",
                                          stat_source_,
                                          true));
    }

    // Prepend CPU utilization to the already-parsed memory metrics for one coherent snapshot.
    const auto active_delta = static_cast<double>(total_delta - idle_delta);
    const double cpu_usage_percent = 100.0 * active_delta / static_cast<double>(total_delta);
    metrics->insert(metrics->begin(),
                    make_metric(MetricSample{
                        .name_ = "cpu_usage_percent",
                        .value_ = cpu_usage_percent,
                        .timestamp_ms_ = timestamp_ms,
                    }));
    return metrics;
}

std::expected<std::string, MetricCollectionError>
ProcMetricCollector::read_source(std::string_view source) {
    // Open the requested procfs file and classify a missing or inaccessible source as retryable.
    std::ifstream input{std::string{source}};
    if (!input) {
        return std::unexpected(make_error(MetricCollectionErrorCode::kReadFailed,
                                          "open_metric_source",
                                          std::string{source},
                                          true));
    }

    // Own the complete snapshot so parsing is independent of the file stream lifetime.
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::expected<ProcMetricCollector::CpuCounters, MetricCollectionError>
ProcMetricCollector::parse_cpu_counters(SourceSnapshot snapshot) {
    // Validate that the aggregate CPU row is the first record in the supplied snapshot.
    std::istringstream input{std::string{snapshot.contents}};
    std::string cpu_label;
    input >> cpu_label;
    if (cpu_label != "cpu") {
        return std::unexpected(make_error(MetricCollectionErrorCode::kMalformedSource,
                                          "parse_cpu_counters",
                                          std::string{snapshot.source},
                                          false));
    }

    // Read the supported Linux CPU fields and require the four core counters.
    std::array<std::uint64_t, 8> fields{};
    std::size_t field_count = 0;
    while (field_count < fields.size() && input >> fields.at(field_count)) {
        ++field_count;
    }
    if (field_count < 4) {
        return std::unexpected(make_error(MetricCollectionErrorCode::kMalformedSource,
                                          "parse_cpu_counters",
                                          std::string{snapshot.source},
                                          false));
    }

    // Sum cumulative counters with explicit overflow protection.
    std::uint64_t total = 0;
    for (const std::uint64_t field : std::span{fields}.first(field_count)) {
        if (field > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::unexpected(make_error(MetricCollectionErrorCode::kMalformedSource,
                                              "parse_cpu_counters",
                                              std::string{snapshot.source},
                                              false));
        }
        total += field;
    }

    // Combine idle and I/O wait only after verifying that their sum is representable.
    if (fields[4] > std::numeric_limits<std::uint64_t>::max() - fields[3]) {
        return std::unexpected(make_error(MetricCollectionErrorCode::kMalformedSource,
                                          "parse_cpu_counters",
                                          std::string{snapshot.source},
                                          false));
    }
    return CpuCounters{.total = total, .idle = fields[3] + fields[4]};
}

std::expected<std::vector<Metric>, MetricCollectionError>
ProcMetricCollector::parse_memory_metrics(SourceSnapshot snapshot, std::int64_t timestamp_ms) {
    // Extract only the total and available fields needed for the approved memory metrics.
    std::istringstream input{std::string{snapshot.contents}};
    std::optional<std::uint64_t> total_kb;
    std::optional<std::uint64_t> available_kb;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream line_input{line};
        std::string key;
        std::uint64_t value = 0;
        std::string unit;
        line_input >> key;
        if (key != "MemTotal:" && key != "MemAvailable:") {
            continue;
        }
        if (!(line_input >> value >> unit) || unit != "kB") {
            return std::unexpected(make_error(MetricCollectionErrorCode::kMalformedSource,
                                              "parse_memory_metrics",
                                              std::string{snapshot.source},
                                              false));
        }
        if (key == "MemTotal:") {
            total_kb = value;
        } else {
            available_kb = value;
        }
    }

    // Validate required fields, their relationship, and the kB-to-byte conversion boundary.
    if (!total_kb || !available_kb || *total_kb == 0 || *available_kb > *total_kb ||
        *total_kb > std::numeric_limits<std::uint64_t>::max() / kBytesPerKilobyte) {
        return std::unexpected(make_error(MetricCollectionErrorCode::kMalformedSource,
                                          "parse_memory_metrics",
                                          std::string{snapshot.source},
                                          false));
    }

    // Derive used bytes and utilization from the validated absolute counters.
    const std::uint64_t total_bytes = *total_kb * kBytesPerKilobyte;
    const std::uint64_t available_bytes = *available_kb * kBytesPerKilobyte;
    const std::uint64_t used_bytes = total_bytes - available_bytes;
    const double usage_percent =
        100.0 * static_cast<double>(used_bytes) / static_cast<double>(total_bytes);

    // Emit all memory values with one timestamp so downstream batches represent one snapshot.
    std::vector<Metric> metrics;
    metrics.reserve(3);
    metrics.push_back(make_metric(MetricSample{
        .name_ = "memory_used_bytes",
        .value_ = static_cast<double>(used_bytes),
        .timestamp_ms_ = timestamp_ms,
    }));
    metrics.push_back(make_metric(MetricSample{
        .name_ = "memory_available_bytes",
        .value_ = static_cast<double>(available_bytes),
        .timestamp_ms_ = timestamp_ms,
    }));
    metrics.push_back(make_metric(MetricSample{
        .name_ = "memory_usage_percent",
        .value_ = usage_percent,
        .timestamp_ms_ = timestamp_ms,
    }));
    return metrics;
}

} // namespace pulsemesh
