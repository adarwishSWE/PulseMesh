// Tests PendingBatchQueue FIFO, ACK, retry, overflow, validation, and concurrency behavior.

#include "cpp/client/impl/pending_batch_queue/pending_batch_queue.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace pulsemesh {
namespace {

MetricBatch make_batch(std::string batch_id) {
    MetricBatch batch;
    batch.set_client_id("agent-a");
    batch.set_batch_id(std::move(batch_id));
    Metric* metric = batch.add_metrics();
    metric->set_name("cpu");
    metric->set_value(1.0);
    return batch;
}

constexpr int kBatchesPerProducer = 200;

struct ConcurrentQueueTestState {
    PendingBatchQueue queue_{1024};
    std::atomic<int> producers_finished_{0};
    std::atomic<int> unexpected_enqueue_results_{0};
    std::atomic<bool> timed_out_{false};
};

void produce_batches(ConcurrentQueueTestState& state, std::string_view prefix) {
    for (int index = 0; index < kBatchesPerProducer; ++index) {
        if (state.queue_.enqueue(make_batch(std::string{prefix} + std::to_string(index))) !=
            EnqueueResult::kEnqueued) {
            state.unexpected_enqueue_results_.fetch_add(1);
        }
    }
    state.producers_finished_.fetch_add(1);
}

void drain_batches(ConcurrentQueueTestState& state) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (state.producers_finished_.load() != 2 || state.queue_.size() != 0) {
        if (const auto batch = state.queue_.mark_next_in_flight()) {
            static_cast<void>(state.queue_.acknowledge(batch->batch_id()));
        } else {
            std::this_thread::yield();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            state.timed_out_.store(true);
            return;
        }
    }
}

} // namespace

class PendingBatchQueueTest : public ::testing::Test {};

// Verifies FIFO send selection and exact acknowledgement matching so an ACK removes only its
// committed batch without disturbing the next queued batch.
TEST_F(PendingBatchQueueTest, GivenQueuedBatches_WhenSentAndAcknowledged_ThenPreservesFifo) {
    // Given
    PendingBatchQueue queue;
    EXPECT_EQ(queue.enqueue(make_batch("A")), EnqueueResult::kEnqueued);
    EXPECT_EQ(queue.enqueue(make_batch("B")), EnqueueResult::kEnqueued);

    // When
    const auto first = queue.mark_next_in_flight();
    const bool removed = queue.acknowledge("A");
    const auto second = queue.mark_next_in_flight();

    // Then
    if (!first.has_value()) {
        FAIL() << "Expected the first queued batch to be available";
    }
    EXPECT_EQ(first->batch_id(), "A");
    EXPECT_TRUE(removed);
    if (!second.has_value()) {
        FAIL() << "Expected the second queued batch to be available";
    }
    EXPECT_EQ(second->batch_id(), "B");
    EXPECT_FALSE(queue.acknowledge("unknown"));
}

// Verifies the reconnect path: resetting in-flight state makes an unacknowledged batch eligible
// again without changing the stable ID or serialized payload required for safe deduplication.
TEST_F(PendingBatchQueueTest, GivenInFlightBatch_WhenReset_ThenRetriesIdenticalBatch) {
    // Given
    PendingBatchQueue queue;
    EXPECT_EQ(queue.enqueue(make_batch("stable-id")), EnqueueResult::kEnqueued);
    const auto first_send = queue.mark_next_in_flight();
    if (!first_send.has_value()) {
        FAIL() << "Expected the queued batch to be available for its first send";
    }

    // When
    queue.reset_in_flight();
    const auto retry = queue.mark_next_in_flight();

    // Then
    if (!retry.has_value()) {
        FAIL() << "Expected the reset batch to be available for retry";
    }
    EXPECT_EQ(retry->SerializeAsString(), first_send->SerializeAsString());
}

// Verifies the approved overflow policy when a pending entry is available: protect in-flight
// work, drop the oldest pending batch, retain fresher data, and increment drop accounting.
TEST_F(PendingBatchQueueTest, GivenFullQueueWithPending_WhenEnqueued_ThenDropsOldestPending) {
    // Given
    PendingBatchQueue queue{3};
    EXPECT_EQ(queue.enqueue(make_batch("A")), EnqueueResult::kEnqueued);
    EXPECT_EQ(queue.enqueue(make_batch("B")), EnqueueResult::kEnqueued);
    EXPECT_EQ(queue.enqueue(make_batch("C")), EnqueueResult::kEnqueued);
    ASSERT_TRUE(queue.mark_next_in_flight().has_value());

    // When
    const EnqueueResult result = queue.enqueue(make_batch("D"));
    EXPECT_TRUE(queue.acknowledge("A"));
    const auto next = queue.mark_next_in_flight();

    // Then
    EXPECT_EQ(result, EnqueueResult::kDroppedOldest);
    EXPECT_EQ(queue.dropped_batches(), 1);
    EXPECT_FALSE(queue.acknowledge("B"));
    if (!next.has_value()) {
        FAIL() << "Expected the next retained pending batch to be available";
    }
    EXPECT_EQ(next->batch_id(), "C");
}

// Verifies that a full queue rejects the incoming batch when every retained entry is in flight,
// preventing loss of batches that may still receive commit acknowledgements.
TEST_F(PendingBatchQueueTest, GivenFullAllInFlightQueue_WhenEnqueued_ThenRejectsIncoming) {
    // Given
    PendingBatchQueue queue{2};
    EXPECT_EQ(queue.enqueue(make_batch("A")), EnqueueResult::kEnqueued);
    EXPECT_EQ(queue.enqueue(make_batch("B")), EnqueueResult::kEnqueued);
    ASSERT_TRUE(queue.mark_next_in_flight().has_value());
    ASSERT_TRUE(queue.mark_next_in_flight().has_value());

    // When
    const EnqueueResult result = queue.enqueue(make_batch("C"));

    // Then
    EXPECT_EQ(result, EnqueueResult::kRejectedAllInFlight);
    EXPECT_EQ(queue.size(), 2);
    EXPECT_EQ(queue.dropped_batches(), 1);
}

// Verifies thread safety under simultaneous production and acknowledgement by ensuring every
// accepted batch drains within a deadline without unexpected overflow or corrupted queue state.
TEST_F(PendingBatchQueueTest, GivenConcurrentAccess_WhenProducedAndAcknowledged_ThenDrainsSafely) {
    // Given
    ConcurrentQueueTestState state;

    // When
    std::jthread first_producer([&state]() {
        produce_batches(state, "A-");
    });
    std::jthread second_producer([&state]() {
        produce_batches(state, "B-");
    });
    std::jthread consumer([&state]() {
        drain_batches(state);
    });
    first_producer.join();
    second_producer.join();
    consumer.join();

    // Then
    EXPECT_EQ(state.unexpected_enqueue_results_.load(), 0);
    EXPECT_FALSE(state.timed_out_.load());
    EXPECT_EQ(state.queue_.size(), 0);
}

} // namespace pulsemesh
