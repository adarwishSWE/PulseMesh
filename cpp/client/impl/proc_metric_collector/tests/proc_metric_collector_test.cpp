// Tests ProcMetricCollector parsing, CPU baselining/deltas, memory metrics, and errors.

#include "cpp/client/impl/proc_metric_collector/proc_metric_collector.h"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace pulsemesh {
namespace {

const Metric* find_metric(const std::vector<Metric>& metrics, std::string_view name) {
    for (const Metric& metric : metrics) {
        if (metric.name() == name) {
            return &metric;
        }
    }
    return nullptr;
}

MetricCollectionError unknown_source_error(std::string_view source) {
    return MetricCollectionError{
        .code = MetricCollectionErrorCode::kReadFailed,
        .operation = "read_fixture",
        .source = std::string{source},
        .retryable = false,
    };
}

struct FixtureSourceReader {
    std::vector<std::string> stat_snapshots_;
    std::string meminfo_;
    std::size_t next_stat_snapshot_ = 0;

    std::expected<std::string, MetricCollectionError> operator()(std::string_view source) {
        if (source == "stat" && next_stat_snapshot_ < stat_snapshots_.size()) {
            return stat_snapshots_.at(next_stat_snapshot_++);
        }
        if (source == "meminfo") {
            return meminfo_;
        }
        return std::unexpected(unknown_source_error(source));
    }
};

} // namespace

class ProcMetricCollectorTest : public ::testing::Test {};

// Verifies first-sample semantics: cumulative CPU counters establish a baseline while absolute
// memory values are immediately emitted with the caller-provided timestamp.
TEST_F(ProcMetricCollectorTest, GivenFirstSnapshot_WhenCollected_ThenEmitsMemoryAndBaselinesCpu) {
    // Given
    FixtureSourceReader reader{
        .stat_snapshots_ = {"cpu 100 0 100 800 0 0 0 0\n"},
        .meminfo_ = "MemTotal: 1000 kB\nMemAvailable: 400 kB\n",
    };
    ProcMetricCollector collector{reader, "stat", "meminfo"};

    // When
    const auto result =
        collector.collect(std::chrono::system_clock::time_point{std::chrono::milliseconds{1000}});

    // Then
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 3);
    EXPECT_EQ(find_metric(*result, "cpu_usage_percent"), nullptr);
    const Metric* used = find_metric(*result, "memory_used_bytes");
    ASSERT_NE(used, nullptr);
    EXPECT_DOUBLE_EQ(used->value(), 600.0 * 1024.0);
    EXPECT_EQ(used->timestamp_ms(), 1000);
    const Metric* usage = find_metric(*result, "memory_usage_percent");
    ASSERT_NE(usage, nullptr);
    EXPECT_DOUBLE_EQ(usage->value(), 60.0);
}

// Verifies that CPU utilization is calculated from the difference between consecutive cumulative
// snapshots, producing the expected active-to-total percentage.
TEST_F(ProcMetricCollectorTest, GivenCpuBaseline_WhenNextSnapshotCollected_ThenUsesCounterDelta) {
    // Given
    FixtureSourceReader reader{
        .stat_snapshots_ =
            {
                "cpu 100 0 100 800 0 0 0 0\n",
                "cpu 150 0 150 900 0 0 0 0\n",
            },
        .meminfo_ = "MemTotal: 1000 kB\nMemAvailable: 400 kB\n",
    };
    ProcMetricCollector collector{reader, "stat", "meminfo"};
    ASSERT_TRUE(collector.collect(std::chrono::system_clock::time_point{}).has_value());

    // When
    const auto result =
        collector.collect(std::chrono::system_clock::time_point{std::chrono::milliseconds{1000}});

    // Then
    ASSERT_TRUE(result.has_value());
    const Metric* cpu = find_metric(*result, "cpu_usage_percent");
    ASSERT_NE(cpu, nullptr);
    EXPECT_DOUBLE_EQ(cpu->value(), 50.0);
}

// Verifies the procfs trust boundary by ensuring a snapshot missing MemAvailable is rejected with
// the specific typed parsing error instead of emitting incomplete memory metrics.
TEST_F(ProcMetricCollectorTest, GivenMalformedMeminfo_WhenCollected_ThenReturnsTypedError) {
    // Given
    FixtureSourceReader reader{
        .stat_snapshots_ = {"cpu 100 0 100 800\n"},
        .meminfo_ = "MemTotal: 1000 kB\n",
    };
    ProcMetricCollector collector{reader, "stat", "meminfo"};

    // When
    const auto result = collector.collect(std::chrono::system_clock::time_point{});

    // Then
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, MetricCollectionErrorCode::kMalformedSource);
    EXPECT_EQ(result.error().operation, "parse_memory_metrics");
}

} // namespace pulsemesh
