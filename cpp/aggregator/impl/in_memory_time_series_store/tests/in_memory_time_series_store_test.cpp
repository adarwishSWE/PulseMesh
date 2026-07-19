// Tests InMemoryTimeSeriesStore batch commits, deduplication, retention, queries, pagination,
// event replay, validation, and concurrent access.

#include "cpp/aggregator/impl/in_memory_time_series_store/in_memory_time_series_store.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace pulsemesh {
namespace {

struct MetricValue {
    double value_;
};

struct MetricTimestampMs {
    std::int64_t value_;
};

struct MetricTimeRange {
    std::int64_t from_ms_;
    std::int64_t to_ms_;
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

MetricBatch make_batch(std::string batch_id, std::vector<Metric> metrics,
                       std::string client_id = "agent-a") {
    MetricBatch batch;
    batch.set_client_id(std::move(client_id));
    batch.set_batch_id(std::move(batch_id));
    for (Metric& metric : metrics) {
        *batch.add_metrics() = std::move(metric);
    }
    return batch;
}

RangeRequest make_range_request(const std::string& metric_name, MetricTimeRange range) {
    RangeRequest request;
    request.set_metric_name(metric_name);
    request.set_from_ms(range.from_ms_);
    request.set_to_ms(range.to_ms_);
    return request;
}

} // namespace

class InMemoryTimeSeriesStoreTest : public ::testing::Test {};

// Verifies that an inclusive range returns every committed sample in bounds and calculates
// average, minimum, and maximum from the complete matching set.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenCommittedMetrics_WhenQueriedByRange_ThenReturnsCorrectSubset) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto commit = store.commit_batch(
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{50.0}, MetricTimestampMs{1000}),
                       make_metric("cpu", MetricValue{60.0}, MetricTimestampMs{2000}),
                       make_metric("cpu", MetricValue{70.0}, MetricTimestampMs{3000}),
                   }));
    ASSERT_TRUE(commit.has_value());

    const RangeRequest request =
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 1000, .to_ms_ = 3000});

    // When
    const auto result = store.query_range(request);

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the range query to succeed";
    }
    EXPECT_EQ(result->metrics_size(), 3);
    EXPECT_DOUBLE_EQ(result->avg(), 60.0);
    EXPECT_DOUBLE_EQ(result->max(), 70.0);
    EXPECT_DOUBLE_EQ(result->min(), 50.0);
}

// Verifies bounded retention by ensuring a commit beyond per-metric capacity evicts the oldest
// timestamp while retaining newer samples in query order.
TEST_F(InMemoryTimeSeriesStoreTest, GivenCapacityExceeded_WhenCommitted_ThenEvictsOldestMetric) {
    // Given
    InMemoryTimeSeriesStore store{2};

    // When
    const auto commit = store.commit_batch(
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}),
                       make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}),
                       make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{3000}),
                   }));
    ASSERT_TRUE(commit.has_value());
    const auto result = store.query_range(
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000}));

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the retained range query to succeed";
    }
    ASSERT_EQ(result->metrics_size(), 2);
    EXPECT_DOUBLE_EQ(result->metrics(0).value(), 20.0);
    EXPECT_DOUBLE_EQ(result->metrics(1).value(), 30.0);
}

// Verifies that tag filters select matching key/value pairs and that aggregates exclude every
// retained sample outside the filtered subset.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenTaggedMetrics_WhenFilteredByTags_ThenReturnsMatchingSubset) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto commit = store.commit_batch(make_batch(
        "batch-1",
        {
            make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}, {{"host", "a"}}),
            make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}, {{"host", "b"}}),
            make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{3000}, {{"host", "a"}}),
        }));
    ASSERT_TRUE(commit.has_value());
    RangeRequest request =
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000});
    (*request.mutable_tags_filter())["host"] = "a";

    // When
    const auto result = store.query_range(request);

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the tag-filtered query to succeed";
    }
    ASSERT_EQ(result->metrics_size(), 2);
    EXPECT_DOUBLE_EQ(result->metrics(0).value(), 10.0);
    EXPECT_DOUBLE_EQ(result->metrics(1).value(), 30.0);
    EXPECT_DOUBLE_EQ(result->avg(), 20.0);
}

// Verifies that a delayed metric is placed according to its timestamp rather than its later
// commit position, preserving chronological range results.
TEST_F(InMemoryTimeSeriesStoreTest, GivenOutOfOrderTimestamp_WhenCommitted_ThenMetricIsSorted) {
    // Given
    InMemoryTimeSeriesStore store;

    // When
    const auto commit = store.commit_batch(
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{2000}),
                       make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{3000}),
                       make_metric("cpu", MetricValue{15.0}, MetricTimestampMs{1500}),
                   }));
    ASSERT_TRUE(commit.has_value());
    const auto result = store.query_range(
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000}));

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the ordered range query to succeed";
    }
    ASSERT_EQ(result->metrics_size(), 3);
    EXPECT_DOUBLE_EQ(result->metrics(0).value(), 15.0);
    EXPECT_DOUBLE_EQ(result->metrics(1).value(), 10.0);
    EXPECT_DOUBLE_EQ(result->metrics(2).value(), 20.0);
}

// Verifies that equal timestamps use commit event order as a stable tie-breaker, which prevents
// pagination and repeated queries from reordering indistinguishable timestamps.
TEST_F(InMemoryTimeSeriesStoreTest, GivenEqualTimestamps_WhenCommitted_ThenPreservesEventOrder) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto commit = store.commit_batch(
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}),
                       make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{1000}),
                       make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{1000}),
                   }));
    ASSERT_TRUE(commit.has_value());

    // When
    const auto result = store.query_range(
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000}));

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the equal-timestamp query to succeed";
    }
    ASSERT_EQ(result->metrics_size(), 3);
    EXPECT_DOUBLE_EQ(result->metrics(0).value(), 10.0);
    EXPECT_DOUBLE_EQ(result->metrics(1).value(), 20.0);
    EXPECT_DOUBLE_EQ(result->metrics(2).value(), 30.0);
}

// Verifies constructor fallback semantics by proving that zero capacity retains the same samples
// as the documented default capacity.
TEST_F(InMemoryTimeSeriesStoreTest, GivenZeroCapacity_WhenCommitted_ThenUsesDefaultCapacity) {
    // Given
    InMemoryTimeSeriesStore store_with_zero{0};
    InMemoryTimeSeriesStore store_with_default;
    std::vector<Metric> metrics;
    metrics.reserve(5);
    for (int index = 0; index < 5; ++index) {
        metrics.push_back(make_metric("cpu",
                                      MetricValue{static_cast<double>(index)},
                                      MetricTimestampMs{1000 + index}));
    }

    // When
    const auto zero_commit = store_with_zero.commit_batch(make_batch("batch-1", metrics));
    const auto default_commit =
        store_with_default.commit_batch(make_batch("batch-1", std::move(metrics)));
    ASSERT_TRUE(zero_commit.has_value());
    ASSERT_TRUE(default_commit.has_value());
    const RangeRequest request =
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000});
    const auto zero_result = store_with_zero.query_range(request);
    const auto default_result = store_with_default.query_range(request);

    // Then
    if (!zero_result.has_value() || !default_result.has_value()) {
        FAIL() << "Expected both default-capacity queries to succeed";
    }
    EXPECT_EQ(zero_result->metrics_size(), default_result->metrics_size());
    EXPECT_EQ(zero_result->metrics_size(), 5);
}

// Verifies that committing a metric replaces its untrusted source identity with the owning Agent
// identity and advances the globally committed event cursor.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenValidBatch_WhenCommitted_ThenPublishesServerControlledIdentity) {
    // Given
    InMemoryTimeSeriesStore store;
    Metric cpu = make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000});
    cpu.set_source_client_id("spoofed-client");

    // When
    const auto commit =
        store.commit_batch(make_batch("batch-1", {std::move(cpu)}, "trusted-agent"));
    const auto event_id = store.current_event_id();
    const auto cpu_result = store.query_range(
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000}));

    // Then
    if (!commit.has_value() || !event_id.has_value() || !cpu_result.has_value()) {
        FAIL() << "Expected the committed metric to be visible";
    }
    EXPECT_EQ(*commit, BatchCommitOutcome::kCommitted);
    EXPECT_EQ(*event_id, 1);
    ASSERT_EQ(cpu_result->metrics_size(), 1);
    EXPECT_EQ(cpu_result->metrics(0).source_client_id(), "trusted-agent");
}

// Verifies effectively-once in-memory behavior by ensuring a repeated stable batch ID reports a
// duplicate without advancing event IDs or inserting its payload twice.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenCommittedBatch_WhenRetried_ThenReturnsDuplicateWithoutInsertion) {
    // Given
    InMemoryTimeSeriesStore store;
    const MetricBatch batch =
        make_batch("batch-1", {make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000})});
    const auto first_commit = store.commit_batch(batch);
    ASSERT_TRUE(first_commit.has_value());

    // When
    const auto retry = store.commit_batch(batch);
    const auto event_id = store.current_event_id();
    const auto result = store.query_range(
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000}));

    // Then
    if (!retry.has_value() || !event_id.has_value() || !result.has_value()) {
        FAIL() << "Expected duplicate detection and subsequent reads to succeed";
    }
    EXPECT_EQ(*retry, BatchCommitOutcome::kDuplicate);
    EXPECT_EQ(*event_id, 1);
    EXPECT_EQ(result->metrics_size(), 1);
}

// Verifies all-or-nothing validation by ensuring one malformed metric rejects the complete batch
// before any valid sibling metric or event ID becomes visible.
TEST_F(InMemoryTimeSeriesStoreTest, GivenMalformedMetric_WhenCommitted_ThenRejectsCompleteBatch) {
    // Given
    InMemoryTimeSeriesStore store;
    const MetricBatch batch =
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}),
                       make_metric("memory",
                                   MetricValue{std::numeric_limits<double>::infinity()},
                                   MetricTimestampMs{1000}),
                   });

    // When
    const auto commit = store.commit_batch(batch);
    const auto event_id = store.current_event_id();
    const auto result = store.query_range(
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000}));

    // Then
    ASSERT_FALSE(commit.has_value());
    EXPECT_EQ(commit.error().code, TimeSeriesStoreErrorCode::kInvalidArgument);
    if (!event_id.has_value() || !result.has_value()) {
        FAIL() << "Expected unchanged store state to remain queryable";
    }
    EXPECT_EQ(*event_id, 0);
    EXPECT_EQ(result->metrics_size(), 0);
}

// Verifies snapshot keyset pagination by preserving equal-timestamp order, excluding later
// commits, and repeating complete-range aggregates on every page.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenEqualTimestamps_WhenPaginated_ThenReturnsStableSnapshotPages) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto initial_commit = store.commit_batch(
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}),
                       make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{1000}),
                       make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{1000}),
                   }));
    ASSERT_TRUE(initial_commit.has_value());
    RangeRequest first_request =
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000});
    first_request.set_page_size(2);

    // When
    const auto first_page = store.query_range(first_request);
    if (!first_page.has_value()) {
        FAIL() << "Expected the first page to succeed";
    }
    const auto later_commit = store.commit_batch(
        make_batch("batch-2", {make_metric("cpu", MetricValue{40.0}, MetricTimestampMs{1000})}));
    ASSERT_TRUE(later_commit.has_value());
    RangeRequest second_request = first_request;
    second_request.set_page_token(first_page->next_page_token());
    const auto second_page = store.query_range(second_request);

    // Then
    if (!second_page.has_value()) {
        FAIL() << "Expected the continuation page to succeed";
    }
    ASSERT_EQ(first_page->metrics_size(), 2);
    EXPECT_DOUBLE_EQ(first_page->metrics(0).value(), 10.0);
    EXPECT_DOUBLE_EQ(first_page->metrics(1).value(), 20.0);
    EXPECT_FALSE(first_page->next_page_token().empty());
    EXPECT_DOUBLE_EQ(first_page->avg(), 20.0);
    ASSERT_EQ(second_page->metrics_size(), 1);
    EXPECT_DOUBLE_EQ(second_page->metrics(0).value(), 30.0);
    EXPECT_TRUE(second_page->next_page_token().empty());
    EXPECT_DOUBLE_EQ(second_page->avg(), 20.0);
}

// Verifies that an invalid opaque continuation is rejected explicitly instead of silently
// restarting the query and returning a duplicated first page.
TEST_F(InMemoryTimeSeriesStoreTest, GivenMalformedPageToken_WhenQueried_ThenReturnsTypedError) {
    // Given
    InMemoryTimeSeriesStore store;
    RangeRequest request =
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000});
    request.set_page_token("not-a-valid-token");

    // When
    const auto result = store.query_range(request);

    // Then
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, TimeSeriesStoreErrorCode::kInvalidArgument);
    EXPECT_EQ(result.error().context, "page_token");
}

// Verifies client filtering against server-controlled identities so samples from another Agent
// cannot enter a client-specific historical result.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenMultipleAgents_WhenFilteredByClient_ThenReturnsOwningAgentOnly) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto first_commit = store.commit_batch(
        make_batch("batch-1",
                   {make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000})},
                   "agent-a"));
    const auto second_commit = store.commit_batch(
        make_batch("batch-2",
                   {make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000})},
                   "agent-b"));
    ASSERT_TRUE(first_commit.has_value());
    ASSERT_TRUE(second_commit.has_value());
    RangeRequest request =
        make_range_request("cpu", MetricTimeRange{.from_ms_ = 0, .to_ms_ = 5000});
    request.set_client_id("agent-a");

    // When
    const auto result = store.query_range(request);

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the client-filtered query to succeed";
    }
    ASSERT_EQ(result->metrics_size(), 1);
    EXPECT_DOUBLE_EQ(result->metrics(0).value(), 10.0);
    EXPECT_EQ(result->metrics(0).source_client_id(), "agent-a");
}

// Verifies event-cursor semantics by filtering across metric buffers, ordering by global commit
// ID, and respecting the caller's explicit fetch bound.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenCommittedEvents_WhenQueriedAfterCursor_ThenReturnsBoundedOrder) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto first_commit = store.commit_batch(
        make_batch("batch-1",
                   {
                       make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}),
                       make_metric("memory", MetricValue{50.0}, MetricTimestampMs{1000}),
                   }));
    const auto second_commit = store.commit_batch(
        make_batch("batch-2", {make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000})}));
    ASSERT_TRUE(first_commit.has_value());
    ASSERT_TRUE(second_commit.has_value());
    SubscribeRequest request;
    request.set_metric_name("cpu");

    // When
    const auto first_page = store.query_events_after(request, 1);
    request.set_after_event_id(1);
    const auto second_page = store.query_events_after(request, 10);

    // Then
    if (!first_page.has_value() || !second_page.has_value()) {
        FAIL() << "Expected both event-cursor reads to succeed";
    }
    ASSERT_EQ(first_page->size(), 1);
    EXPECT_EQ(first_page->front().event_id(), 1);
    EXPECT_DOUBLE_EQ(first_page->front().metric().value(), 10.0);
    ASSERT_EQ(second_page->size(), 1);
    EXPECT_EQ(second_page->front().event_id(), 3);
    EXPECT_DOUBLE_EQ(second_page->front().metric().value(), 20.0);
}

// Verifies synchronization under concurrent batch commits and range queries by completing both
// workers without failed operations and retaining every committed metric.
TEST_F(InMemoryTimeSeriesStoreTest,
       GivenConcurrentAccess_WhenCommittingAndQuerying_ThenRemainsConsistent) {
    // Given
    InMemoryTimeSeriesStore store;
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    const RangeRequest request = make_range_request(
        "cpu",
        MetricTimeRange{.from_ms_ = 0, .to_ms_ = std::numeric_limits<std::int64_t>::max()});

    // When
    std::thread writer([&store, &done, &failed]() {
        for (int index = 0; index < 200; ++index) {
            const auto commit =
                store.commit_batch(make_batch("batch-" + std::to_string(index),
                                              {make_metric("cpu",
                                                           MetricValue{static_cast<double>(index)},
                                                           MetricTimestampMs{index + 1})}));
            if (!commit.has_value()) {
                failed.store(true);
                break;
            }
        }
        done.store(true);
    });

    std::thread reader([&store, &request, &done, &failed]() {
        while (!done.load()) {
            if (!store.query_range(request).has_value()) {
                failed.store(true);
                return;
            }
        }
    });

    writer.join();
    reader.join();
    const auto result = store.query_range(request);

    // Then
    EXPECT_FALSE(failed.load());
    if (!result.has_value()) {
        FAIL() << "Expected the final concurrent range query to succeed";
    }
    EXPECT_EQ(result->metrics_size(), 200);
}

// Verifies that latest lookup scans newest-first and returns the most recent sample satisfying
// both tag and client filters rather than the newest unfiltered metric.
TEST_F(InMemoryTimeSeriesStoreTest, GivenMatchingMetrics_WhenLatestQueried_ThenReturnsNewestMatch) {
    // Given
    InMemoryTimeSeriesStore store;
    const auto first_commit = store.commit_batch(make_batch(
        "batch-1",
        {make_metric("cpu", MetricValue{10.0}, MetricTimestampMs{1000}, {{"host", "a"}})},
        "agent-a"));
    const auto second_commit = store.commit_batch(make_batch(
        "batch-2",
        {make_metric("cpu", MetricValue{20.0}, MetricTimestampMs{2000}, {{"host", "a"}})},
        "agent-a"));
    const auto third_commit = store.commit_batch(make_batch(
        "batch-3",
        {make_metric("cpu", MetricValue{30.0}, MetricTimestampMs{3000}, {{"host", "a"}})},
        "agent-b"));
    ASSERT_TRUE(first_commit.has_value());
    ASSERT_TRUE(second_commit.has_value());
    ASSERT_TRUE(third_commit.has_value());
    LatestRequest request;
    request.set_metric_name("cpu");
    request.set_client_id("agent-a");
    (*request.mutable_tags_filter())["host"] = "a";

    // When
    const auto result = store.query_latest(request);

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected the latest query to succeed";
    }
    const std::optional<LatestResponse>& latest = *result;
    if (!latest.has_value()) {
        FAIL() << "Expected a matching latest metric";
    }
    EXPECT_DOUBLE_EQ(latest->metric().value(), 20.0);
    EXPECT_EQ(latest->metric().source_client_id(), "agent-a");
}

// Verifies that a successful latest query with no matching data uses optional absence rather than
// fabricating a metric or returning a storage failure.
TEST_F(InMemoryTimeSeriesStoreTest, GivenNoMatchingData_WhenLatestQueried_ThenReturnsNullopt) {
    // Given
    InMemoryTimeSeriesStore store;
    LatestRequest request;
    request.set_metric_name("cpu");

    // When
    const auto result = store.query_latest(request);

    // Then
    if (!result.has_value()) {
        FAIL() << "Expected an empty latest query to succeed";
    }
    EXPECT_FALSE(result->has_value());
}

} // namespace pulsemesh
