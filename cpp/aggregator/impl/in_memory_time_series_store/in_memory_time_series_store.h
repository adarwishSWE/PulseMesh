#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpp/aggregator/i_time_series_store.h"

namespace pulsemesh {

// Thread-safe bounded development backend for the shared time-series storage contract.
// Per-metric buffers are ordered by (timestamp_ms, event_id), including out-of-order arrivals.
class InMemoryTimeSeriesStore : public ITimeSeriesStore {
public:
    static constexpr std::size_t kDefaultCapacityPerMetric = 10000;

    // capacity_per_metric == 0 is treated as kDefaultCapacityPerMetric.
    explicit InMemoryTimeSeriesStore(std::size_t capacity_per_metric = kDefaultCapacityPerMetric);
    ~InMemoryTimeSeriesStore() override = default;

    [[nodiscard]] std::expected<BatchCommitOutcome, TimeSeriesStoreError>
    commit_batch(const MetricBatch& batch) override;

    [[nodiscard]] std::expected<RangeResponse, TimeSeriesStoreError>
    query_range(const RangeRequest& request) const override;

    [[nodiscard]] std::expected<std::optional<LatestResponse>, TimeSeriesStoreError>
    query_latest(const LatestRequest& request) const override;

    [[nodiscard]] std::expected<std::int64_t, TimeSeriesStoreError>
    current_event_id() const override;

    [[nodiscard]] std::expected<std::vector<MetricEvent>, TimeSeriesStoreError>
    query_events_after(const SubscribeRequest& request, std::uint32_t fetch_limit) const override;

    InMemoryTimeSeriesStore(const InMemoryTimeSeriesStore&) = delete;
    InMemoryTimeSeriesStore& operator=(const InMemoryTimeSeriesStore&) = delete;
    InMemoryTimeSeriesStore(InMemoryTimeSeriesStore&&) = delete;
    InMemoryTimeSeriesStore& operator=(InMemoryTimeSeriesStore&&) = delete;

private:
    struct StoredMetric {
        Metric metric_;
        std::int64_t event_id_;
    };

    std::size_t capacity_per_metric_;
    // Protects metric_buffers_, committed_batch_ids_, and current_event_id_.
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::deque<StoredMetric>> metric_buffers_;
    std::unordered_set<std::string> committed_batch_ids_;
    std::int64_t current_event_id_ = 0;
};

} // namespace pulsemesh
