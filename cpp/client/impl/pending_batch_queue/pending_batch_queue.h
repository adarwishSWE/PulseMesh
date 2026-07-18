#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>

#include "proto/metrics.pb.h"

namespace pulsemesh {

enum class EnqueueResult {
    kEnqueued,
    kDroppedOldest,
    kRejectedAllInFlight,
    kInvalidBatch,
    kDuplicateBatch,
};

// Thread-safe bounded owner of batches that have not received a commit acknowledgement.
class PendingBatchQueue {
public:
    static constexpr std::size_t kDefaultCapacity = 256;

    explicit PendingBatchQueue(std::size_t capacity = kDefaultCapacity);

    [[nodiscard]] EnqueueResult enqueue(MetricBatch batch);
    [[nodiscard]] std::optional<MetricBatch> mark_next_in_flight();
    [[nodiscard]] bool acknowledge(std::string_view batch_id);
    void reset_in_flight();

    std::size_t size() const;
    std::uint64_t dropped_batches() const noexcept;

    PendingBatchQueue(const PendingBatchQueue&) = delete;
    PendingBatchQueue& operator=(const PendingBatchQueue&) = delete;
    PendingBatchQueue(PendingBatchQueue&&) = delete;
    PendingBatchQueue& operator=(PendingBatchQueue&&) = delete;

private:
    struct PendingBatch {
        MetricBatch batch;
        bool in_flight = false;
    };

    const std::size_t capacity_;
    mutable std::mutex mutex_; // protects batches_
    std::deque<PendingBatch> batches_;
    std::atomic<std::uint64_t> dropped_batches_{0};
};

} // namespace pulsemesh
