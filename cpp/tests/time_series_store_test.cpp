// Tests TimeSeriesStore: insert, range query with aggregates, capacity eviction,
// tag filtering, out-of-order sorting, and concurrent insert/query.

#include "cpp/aggregator/impl/time_series_store.h"

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

Metric MakeMetric(const std::string& name,
                  double value,
                  int64_t timestamp_ms,
                  const std::map<std::string, std::string>& tags = {}) {
    Metric metric;
    metric.set_name(name);
    metric.set_value(value);
    metric.set_timestamp_ms(timestamp_ms);
    for (const auto& tag : tags) {
        (*metric.mutable_tags())[tag.first] = tag.second;
    }
    return metric;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

class TimeSeriesStoreTest : public ::testing::Test {};

// Given inserted metrics, When queried by range, Then returns correct subset and aggregates.
TEST_F(TimeSeriesStoreTest, GivenMetricsInserted_WhenQueriedByRange_ThenReturnsCorrectSubset) {
    // Given
    TimeSeriesStore store;
    store.Insert(MakeMetric("cpu", 50.0, 1000));
    store.Insert(MakeMetric("cpu", 60.0, 2000));
    store.Insert(MakeMetric("cpu", 70.0, 3000));

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

// Given capacity exceeded, When inserting another point, Then oldest entry is evicted.
TEST_F(TimeSeriesStoreTest, GivenCapacityExceeded_WhenInserting_ThenEvictsOldestEntry) {
    // Given
    TimeSeriesStore store(2);
    store.Insert(MakeMetric("cpu", 10.0, 1000));
    store.Insert(MakeMetric("cpu", 20.0, 2000));

    // When
    store.Insert(MakeMetric("cpu", 30.0, 3000));

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

// Given tagged metrics, When queried with tag filter, Then returns matching subset only.
TEST_F(TimeSeriesStoreTest, GivenTaggedMetrics_WhenFilteredByTags_ThenReturnsMatchingSubset) {
    // Given
    TimeSeriesStore store;
    store.Insert(MakeMetric("cpu", 10.0, 1000, {{"host", "a"}}));
    store.Insert(MakeMetric("cpu", 20.0, 2000, {{"host", "b"}}));
    store.Insert(MakeMetric("cpu", 30.0, 3000, {{"host", "a"}}));

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

// Given out-of-order timestamp, When inserting, Then sample is kept in sorted order.
TEST_F(TimeSeriesStoreTest, GivenOutOfOrderTimestamp_WhenInserting_ThenSampleIsSorted) {
    // Given
    TimeSeriesStore store;
    store.Insert(MakeMetric("cpu", 10.0, 2000));
    store.Insert(MakeMetric("cpu", 20.0, 3000));

    // When
    store.Insert(MakeMetric("cpu", 15.0, 1500));

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

// Given out-of-order timestamp from another host, When inserting, Then sample is kept.
TEST_F(TimeSeriesStoreTest, GivenOutOfOrderTaggedMetric_WhenInserting_ThenSampleIsSorted) {
    // Given
    TimeSeriesStore store;
    store.Insert(MakeMetric("cpu", 10.0, 3000, {{"host", "a"}}));

    // When
    store.Insert(MakeMetric("cpu", 20.0, 2000, {{"host", "b"}}));

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

// Given zero capacity, When inserting, Then uses default capacity like default constructor.
TEST_F(TimeSeriesStoreTest, GivenZeroCapacity_WhenInserting_ThenUsesDefaultCapacity) {
    // Given
    TimeSeriesStore store_with_zero(0);
    TimeSeriesStore store_with_default;

    // When
    for (int i = 0; i < 5; ++i) {
        store_with_zero.Insert(MakeMetric("cpu", static_cast<double>(i), 1000 + i));
        store_with_default.Insert(MakeMetric("cpu", static_cast<double>(i), 1000 + i));
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

// Given equal timestamps, When inserting, Then points are stored in arrival order.
TEST_F(TimeSeriesStoreTest, GivenEqualTimestamps_WhenInserting_ThenPreservesArrivalOrder) {
    // Given
    TimeSeriesStore store;
    store.Insert(MakeMetric("cpu", 10.0, 1000));
    store.Insert(MakeMetric("cpu", 20.0, 1000));
    store.Insert(MakeMetric("cpu", 30.0, 1000));

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

// Given concurrent writers and readers, When operating simultaneously, Then store remains consistent.
TEST_F(TimeSeriesStoreTest, GivenConcurrentAccess_WhenInsertingAndQuerying_ThenNoDataRace) {
    // Given
    TimeSeriesStore store;
    std::atomic<bool> done{false};

    // When
    std::thread writer([&store, &done]() {
        for (int i = 0; i < 1000; ++i) {
            store.Insert(MakeMetric("cpu", static_cast<double>(i), NowMs() + i));
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

// Given metrics exist, When QueryLatest is called with matching filter, Then returns newest point.
TEST_F(TimeSeriesStoreTest, GivenMetricsInserted_WhenQueryLatest_ThenReturnsNewestMatch) {
    // Given
    TimeSeriesStore store;
    store.Insert(MakeMetric("cpu", 10.0, 1000, {{"host", "a"}}));
    store.Insert(MakeMetric("cpu", 20.0, 2000, {{"host", "a"}}));

    LatestRequest request;
    request.set_metric_name("cpu");
    (*request.mutable_tags_filter())["host"] = "a";

    // When
    const std::optional<LatestResponse> response = store.QueryLatest(request);

    // Then
    ASSERT_TRUE(response.has_value());
    EXPECT_DOUBLE_EQ(response->metric().value(), 20.0);
}

// Given no matching metric, When QueryLatest is called, Then returns nullopt.
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

}  // namespace pulsemesh
