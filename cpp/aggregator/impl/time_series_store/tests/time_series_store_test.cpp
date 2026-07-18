// Tests TimeSeriesStore: insert, range query with aggregates, capacity eviction,
// tag filtering, out-of-order sorting, and concurrent insert/query.

#include "cpp/aggregator/impl/time_series_store/time_series_store.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace pulsemesh {
namespace {

struct MetricValue {
    double value_;
};

struct MetricTimestampMs {
    int64_t value_;
};

Metric make_metric(const std::string& name, MetricValue value, MetricTimestampMs timestamp_ms,
                   const std::map<std::string, std::string>& tags = {}) {
    Metric metric;
    metric.set_name(name);
    metric.set_value(value.value_);
    metric.set_timestamp_ms(timestamp_ms.value_);
    for (const auto& tag : tags) {
        (*metric.mutable_tags())[tag.first] = tag.second;
    }
    return metric;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

class TimeSeriesStoreTest : public ::testing::Test {};

// Verifies that an inclusive time-range query returns every retained sample in bounds and computes
// average, minimum, and maximum from exactly those returned values.
TEST_F(TimeSeriesStoreTest, GivenMetricsInserted_WhenQueriedByRange_ThenReturnsCorrectSubset) {
    // Given
    TimeSeriesStore store;
    store.Insert(make_metric("cpu", MetricValue{50.0}, MetricTimestampMs{1000}));
    store.Insert(make_metric("cpu", MetricValue{60.0}, MetricTimestampMs{2000}));
    store.Insert(make_metric("cpu", MetricValue{70.0}, MetricTimestampMs{3000}));

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(1000);
    request.set_to_ms(3000);

    // When
    const RangeResponse response = store.QueryRange(request);

    // Then
    EXPECT_EQ(response.metrics_size(), 3);
    EXPECT_DOUBLE_EQ(response.avg(), 60.0);
    EXPECT_DOUBLE_EQ(response.max(), 70.0);
    EXPECT_DOUBLE_EQ(response.min(), 50.0);
}

// Verifies bounded retention by ensuring insertion beyond per-metric capacity evicts the oldest
// timestamp while preserving newer samples in query order.
TEST_F(TimeSeriesStoreTest, GivenCapacityExceeded_WhenInserting_ThenEvictsOldestEntry) {
    // Given
    TimeSeriesStore store(2);
    store.Insert(make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}));
    store.Insert(make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}));

    // When
    store.Insert(make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{3000}));

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(5000);
    const RangeResponse response = store.QueryRange(request);

    // Then
    ASSERT_EQ(response.metrics_size(), 2);
    EXPECT_DOUBLE_EQ(response.metrics(0).value(), 20.0);
    EXPECT_DOUBLE_EQ(response.metrics(1).value(), 30.0);
}

// Verifies that range tag filters require matching key/value pairs and that aggregates are
// calculated only from the filtered subset rather than all samples in the time range.
TEST_F(TimeSeriesStoreTest, GivenTaggedMetrics_WhenFilteredByTags_ThenReturnsMatchingSubset) {
    // Given
    TimeSeriesStore store;
    store.Insert(make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}, {{"host", "a"}}));
    store.Insert(make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}, {{"host", "b"}}));
    store.Insert(make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{3000}, {{"host", "a"}}));

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(5000);
    (*request.mutable_tags_filter())["host"] = "a";

    // When
    const RangeResponse response = store.QueryRange(request);

    // Then
    ASSERT_EQ(response.metrics_size(), 2);
    EXPECT_DOUBLE_EQ(response.metrics(0).value(), 10.0);
    EXPECT_DOUBLE_EQ(response.metrics(1).value(), 30.0);
    EXPECT_DOUBLE_EQ(response.avg(), 20.0);
}

// Verifies that an older sample arriving after newer samples is inserted at its timestamp-ordered
// position instead of being appended or discarded.
TEST_F(TimeSeriesStoreTest, GivenOutOfOrderTimestamp_WhenInserting_ThenSampleIsSorted) {
    // Given
    TimeSeriesStore store;
    store.Insert(make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{2000}));
    store.Insert(make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{3000}));

    // When
    store.Insert(make_metric("cpu", MetricValue{15.0}, MetricTimestampMs{1500}));

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(5000);
    const RangeResponse response = store.QueryRange(request);

    // Then
    ASSERT_EQ(response.metrics_size(), 3);
    EXPECT_DOUBLE_EQ(response.metrics(0).value(), 15.0);
    EXPECT_DOUBLE_EQ(response.metrics(1).value(), 10.0);
    EXPECT_DOUBLE_EQ(response.metrics(2).value(), 20.0);
}

// Verifies that out-of-order insertion remains timestamp ordered regardless of tag differences,
// since tags filter samples but do not partition the metric's retained buffer.
TEST_F(TimeSeriesStoreTest, GivenOutOfOrderTaggedMetric_WhenInserting_ThenSampleIsSorted) {
    // Given
    TimeSeriesStore store;
    store.Insert(make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{3000}, {{"host", "a"}}));

    // When
    store.Insert(make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}, {{"host", "b"}}));

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(5000);
    const RangeResponse response = store.QueryRange(request);

    // Then
    ASSERT_EQ(response.metrics_size(), 2);
    EXPECT_DOUBLE_EQ(response.metrics(0).value(), 20.0);
    EXPECT_DOUBLE_EQ(response.metrics(1).value(), 10.0);
}

// Verifies constructor fallback semantics by proving that an explicit zero capacity retains the
// same default number of newest samples as the default constructor.
TEST_F(TimeSeriesStoreTest, GivenZeroCapacity_WhenInserting_ThenUsesDefaultCapacity) {
    // Given
    TimeSeriesStore store_with_zero(0);
    TimeSeriesStore store_with_default;

    // When
    for (int i = 0; i < 5; ++i) {
        store_with_zero.Insert(
            make_metric("cpu", MetricValue{static_cast<double>(i)}, MetricTimestampMs{1000 + i}));
        store_with_default.Insert(
            make_metric("cpu", MetricValue{static_cast<double>(i)}, MetricTimestampMs{1000 + i}));
    }

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(5000);

    const RangeResponse zero_response = store_with_zero.QueryRange(request);
    const RangeResponse default_response = store_with_default.QueryRange(request);

    // Then
    EXPECT_EQ(zero_response.metrics_size(), default_response.metrics_size());
    ASSERT_EQ(zero_response.metrics_size(), 5);
}

// Verifies deterministic ordering for equal timestamps by preserving insertion order, which keeps
// repeated range queries stable when timestamps alone cannot distinguish samples.
TEST_F(TimeSeriesStoreTest, GivenEqualTimestamps_WhenInserting_ThenPreservesArrivalOrder) {
    // Given
    TimeSeriesStore store;
    store.Insert(make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}));
    store.Insert(make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{1000}));
    store.Insert(make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{1000}));

    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(5000);

    // When
    const RangeResponse response = store.QueryRange(request);

    // Then
    ASSERT_EQ(response.metrics_size(), 3);
    EXPECT_DOUBLE_EQ(response.metrics(0).value(), 10.0);
    EXPECT_DOUBLE_EQ(response.metrics(1).value(), 20.0);
    EXPECT_DOUBLE_EQ(response.metrics(2).value(), 30.0);
}

// Verifies synchronization under concurrent insert and range-query activity by completing both
// workers without a hang and retaining every inserted sample.
TEST_F(TimeSeriesStoreTest, GivenConcurrentAccess_WhenInsertingAndQuerying_ThenNoDataRace) {
    // Given
    TimeSeriesStore store;
    std::atomic<bool> done{false};

    // When
    std::thread writer([&store, &done]() {
        for (int i = 0; i < 1000; ++i) {
            store.Insert(make_metric("cpu",
                                     MetricValue{static_cast<double>(i)},
                                     MetricTimestampMs{now_ms() + i}));
        }
        done.store(true);
    });

    std::thread reader([&store, &done]() {
        RangeRequest request;
        request.set_metric_name("cpu");
        request.set_from_ms(0);
        request.set_to_ms(std::numeric_limits<int64_t>::max());

        while (!done.load()) {
            const RangeResponse response = store.QueryRange(request);
            EXPECT_GE(response.metrics_size(), 0);
        }
    });

    writer.join();
    reader.join();

    // Then
    RangeRequest request;
    request.set_metric_name("cpu");
    request.set_from_ms(0);
    request.set_to_ms(std::numeric_limits<int64_t>::max());
    const RangeResponse response = store.QueryRange(request);
    EXPECT_EQ(response.metrics_size(), 1000);
}

// Verifies that the latest query scans newest-first and returns the most recent sample satisfying
// all requested tags rather than merely the newest unfiltered metric.
TEST_F(TimeSeriesStoreTest, GivenMetricsInserted_WhenQueryLatest_ThenReturnsNewestMatch) {
    // Given
    TimeSeriesStore store;
    store.Insert(make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}, {{"host", "a"}}));
    store.Insert(make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}, {{"host", "a"}}));

    LatestRequest request;
    request.set_metric_name("cpu");
    (*request.mutable_tags_filter())["host"] = "a";

    // When
    const std::optional<LatestResponse> response = store.QueryLatest(request);

    // Then
    if (!response.has_value()) {
        FAIL() << "Expected the latest matching metric to be available";
    }
    EXPECT_DOUBLE_EQ(response->metric().value(), 20.0);
}

// Verifies that absence of a requested metric is represented as normal optional absence rather
// than a fabricated response or error.
TEST_F(TimeSeriesStoreTest, GivenNoData_WhenQueryLatest_ThenReturnsNullopt) {
    // Given
    TimeSeriesStore store;

    LatestRequest request;
    request.set_metric_name("cpu");

    // When
    const std::optional<LatestResponse> response = store.QueryLatest(request);

    // Then
    EXPECT_FALSE(response.has_value());
}

} // namespace pulsemesh
