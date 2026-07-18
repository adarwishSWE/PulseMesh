#include "cpp/client/impl/batch_builder/batch_builder.h"
#include "cpp/client/impl/pending_batch_queue/pending_batch_queue.h"
#include "cpp/client/impl/proc_metric_collector/proc_metric_collector.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace {

bool collect_and_enqueue(pulsemesh::ProcMetricCollector& collector,
                         pulsemesh::BatchBuilder& builder, pulsemesh::PendingBatchQueue& queue) {
    // Collect one timestamped snapshot from the real Linux metric sources.
    auto metrics = collector.collect(std::chrono::system_clock::now());
    if (!metrics) {
        std::cerr << "[CollectorSmoke] collection failed: " << metrics.error().operation << " ("
                  << metrics.error().source << ")\n";
        return false;
    }

    // Convert the snapshot into a uniquely identified retryable batch.
    auto batch = builder.build(std::move(*metrics));
    if (!batch) {
        std::cerr << "[CollectorSmoke] batch creation failed\n";
        return false;
    }

    // Retain the batch in the bounded queue and expose its identity for manual verification.
    const auto metric_count = static_cast<std::size_t>(batch->metrics_size());
    const std::string batch_id = batch->batch_id();
    if (queue.enqueue(std::move(*batch)) != pulsemesh::EnqueueResult::kEnqueued) {
        std::cerr << "[CollectorSmoke] enqueue failed\n";
        return false;
    }

    std::cout << "[CollectorSmoke] batch_id=" << batch_id << " metrics=" << metric_count << "\n";
    return true;
}

} // namespace

int main() {
    // Assemble the Phase 3 production components with their default settings.
    pulsemesh::ProcMetricCollector collector;
    pulsemesh::BatchBuilder builder{"collector-smoke"};
    pulsemesh::PendingBatchQueue queue;

    // Take the baseline snapshot, then wait long enough for a meaningful CPU counter delta.
    if (!collect_and_enqueue(collector, builder, queue)) {
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    if (!collect_and_enqueue(collector, builder, queue)) {
        return 1;
    }

    // Confirm both batches remain queued for the future gRPC sender.
    std::cout << "[CollectorSmoke] queued_batches=" << queue.size() << "\n";
    return 0;
}
