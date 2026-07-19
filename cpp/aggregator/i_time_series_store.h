#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "proto/metrics.pb.h"

namespace pulsemesh {

enum class BatchCommitOutcome {
    kCommitted,
    kDuplicate,
};

enum class TimeSeriesStoreErrorCode {
    kInvalidArgument,
    kConnection,
    kTransaction,
    kConstraint,
    kQuery,
    kConfiguration,
};

struct TimeSeriesStoreError {
    TimeSeriesStoreErrorCode code;
    std::string operation;
    std::string context;
    bool retryable;
};

// Thread-safe persistence boundary for atomic ingestion, historical queries, and event replay.
class ITimeSeriesStore {
public:
    ITimeSeriesStore() = default;
    virtual ~ITimeSeriesStore() = default;

    // Commits a batch and all of its metrics atomically; an existing batch ID is a successful
    // duplicate outcome rather than a second insertion.
    [[nodiscard]] virtual std::expected<BatchCommitOutcome, TimeSeriesStoreError>
    commit_batch(const MetricBatch& batch) = 0;

    // Returns one stable page and aggregates over the complete snapshot-bounded filtered range.
    [[nodiscard]] virtual std::expected<RangeResponse, TimeSeriesStoreError>
    query_range(const RangeRequest& request) const = 0;

    // Returns nullopt when the query succeeds but no metric matches the requested filters.
    [[nodiscard]] virtual std::expected<std::optional<LatestResponse>, TimeSeriesStoreError>
    query_latest(const LatestRequest& request) const = 0;

    // Returns the latest globally committed event ID, or zero when the store contains no events.
    [[nodiscard]] virtual std::expected<std::int64_t, TimeSeriesStoreError>
    current_event_id() const = 0;

    // Returns at most fetch_limit matching events strictly after request.after_event_id in
    // increasing event-ID order. A zero fetch_limit is invalid.
    [[nodiscard]] virtual std::expected<std::vector<MetricEvent>, TimeSeriesStoreError>
    query_events_after(const SubscribeRequest& request, std::uint32_t fetch_limit) const = 0;

    ITimeSeriesStore(const ITimeSeriesStore&) = delete;
    ITimeSeriesStore& operator=(const ITimeSeriesStore&) = delete;
    ITimeSeriesStore(ITimeSeriesStore&&) = delete;
    ITimeSeriesStore& operator=(ITimeSeriesStore&&) = delete;
};

} // namespace pulsemesh
