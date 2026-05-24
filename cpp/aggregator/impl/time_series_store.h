#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "cpp/aggregator/i_time_series_store.h"

namespace pulsemesh {

// Thread-safe in-memory time-series storage (see time_series_store.cpp).
// Per-metric buffers are kept sorted by timestamp_ms; out-of-order inserts are placed in order.
class TimeSeriesStore : public ITimeSeriesStore {
public:
    static constexpr std::size_t kDefaultCapacityPerMetric = 10000;

    // capacity_per_metric == 0 is treated as kDefaultCapacityPerMetric.
    explicit TimeSeriesStore(
        std::size_t capacity_per_metric = kDefaultCapacityPerMetric);
    ~TimeSeriesStore() override = default;

    void Insert(const Metric& metric) override;

    RangeResponse QueryRange(const RangeRequest& request) const override;

    std::optional<LatestResponse> QueryLatest(const LatestRequest& request) const override;

    TimeSeriesStore(const TimeSeriesStore&)            = delete;
    TimeSeriesStore& operator=(const TimeSeriesStore&) = delete;
    TimeSeriesStore(TimeSeriesStore&&)                 = delete;
    TimeSeriesStore& operator=(TimeSeriesStore&&)      = delete;

private:
    std::size_t capacity_per_metric_;
    mutable std::shared_mutex mutex_;  // protects metric_buffers_
    std::unordered_map<std::string, std::deque<Metric>> metric_buffers_;
};

}  // namespace pulsemesh
