#include "cpp/client/impl/pending_batch_queue/pending_batch_queue.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace pulsemesh {

PendingBatchQueue::PendingBatchQueue(std::size_t capacity)
    : capacity_(capacity == 0 ? kDefaultCapacity : capacity) {}

EnqueueResult PendingBatchQueue::enqueue(MetricBatch batch) {
    // Validate the batch before it can consume bounded queue capacity.
    if (batch.client_id().empty() || batch.batch_id().empty() || batch.metrics().empty()) {
        return EnqueueResult::kInvalidBatch;
    }

    // Reject a repeated batch ID so one logical batch has one queue entry.
    std::scoped_lock lock(mutex_);
    const auto duplicate = std::ranges::find_if(batches_, [&batch](const PendingBatch& entry) {
        return entry.batch.batch_id() == batch.batch_id();
    });
    if (duplicate != batches_.end()) {
        return EnqueueResult::kDuplicateBatch;
    }

    // Make capacity available by dropping the oldest pending entry, while preserving every
    // in-flight batch that may still receive an acknowledgement.
    EnqueueResult result = EnqueueResult::kEnqueued;
    if (batches_.size() == capacity_) {
        const auto oldest_pending = std::ranges::find_if(batches_, [](const PendingBatch& entry) {
            return !entry.in_flight;
        });
        if (oldest_pending == batches_.end()) {
            dropped_batches_.fetch_add(1);
            return EnqueueResult::kRejectedAllInFlight;
        }
        batches_.erase(oldest_pending);
        dropped_batches_.fetch_add(1);
        result = EnqueueResult::kDroppedOldest;
    }

    // Append the accepted batch at the FIFO tail and report whether overflow caused a drop.
    batches_.push_back(PendingBatch{.batch = std::move(batch)});
    return result;
}

std::optional<MetricBatch> PendingBatchQueue::mark_next_in_flight() {
    // Locate the oldest batch that is not already awaiting an acknowledgement.
    std::scoped_lock lock(mutex_);
    const auto next_pending = std::ranges::find_if(batches_, [](const PendingBatch& entry) {
        return !entry.in_flight;
    });
    if (next_pending == batches_.end()) {
        return std::nullopt;
    }

    // Protect the selected entry from overflow before returning its retry-stable payload.
    next_pending->in_flight = true;
    return next_pending->batch;
}

bool PendingBatchQueue::acknowledge(std::string_view batch_id) {
    // Find the retained batch that matches the Aggregator's committed batch ID.
    std::scoped_lock lock(mutex_);
    // ponytail: The linear lookup is bounded by queue capacity (256 by default). Add a stable
    // order hash index only if measurements show materially larger queues make this expensive.
    const auto acknowledged = std::ranges::find_if(batches_, [batch_id](const PendingBatch& entry) {
        return entry.batch.batch_id() == batch_id;
    });
    if (acknowledged == batches_.end()) {
        return false;
    }

    // Remove only the acknowledged batch; every other pending or in-flight entry remains intact.
    batches_.erase(acknowledged);
    return true;
}

void PendingBatchQueue::reset_in_flight() {
    // A disconnected stream makes every unacknowledged batch eligible for an identical retry.
    std::scoped_lock lock(mutex_);
    for (PendingBatch& entry : batches_) {
        entry.in_flight = false;
    }
}

std::size_t PendingBatchQueue::size() const {
    // Read compound queue state under the same mutex used by writers.
    std::scoped_lock lock(mutex_);
    return batches_.size();
}

std::uint64_t PendingBatchQueue::dropped_batches() const noexcept {
    // This independent statistic does not require the queue mutex.
    return dropped_batches_.load();
}

} // namespace pulsemesh
