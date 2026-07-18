#include "cpp/client/impl/batch_builder/batch_builder.h"

#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace pulsemesh {

BatchBuilder::BatchBuilder(std::string client_id)
    : BatchBuilder(std::move(client_id), make_session_nonce()) {}

BatchBuilder::BatchBuilder(std::string client_id, std::uint64_t session_nonce)
    : client_id_(std::move(client_id)), session_nonce_(session_nonce) {}

std::expected<MetricBatch, BatchBuildError> BatchBuilder::build(std::vector<Metric> metrics) {
    // Validate the Agent identity and require at least one sample before assigning an ID.
    if (client_id_.empty()) {
        return std::unexpected(BatchBuildError{.code = BatchBuildErrorCode::kEmptyClientId});
    }
    if (metrics.empty()) {
        return std::unexpected(BatchBuildError{.code = BatchBuildErrorCode::kEmptyMetrics});
    }

    // Validate every metric so malformed values never enter the retry queue.
    for (const Metric& metric : metrics) {
        if (metric.name().empty() || !std::isfinite(metric.value())) {
            return std::unexpected(BatchBuildError{.code = BatchBuildErrorCode::kInvalidMetric});
        }
    }

    // Assign the stable identity before the batch can be queued or sent.
    MetricBatch batch;
    batch.set_client_id(client_id_);
    batch.set_batch_id(next_batch_id());

    // Transfer the validated payload into the completed batch without copying each metric.
    for (Metric& metric : metrics) {
        *batch.add_metrics() = std::move(metric);
    }
    return batch;
}

std::uint64_t BatchBuilder::make_session_nonce() {
    // Give each Agent process session a random namespace for its monotonic sequence numbers.
    std::random_device random;
    const auto high = static_cast<std::uint64_t>(random()) << 32U;
    return high ^ static_cast<std::uint64_t>(random());
}

std::string BatchBuilder::next_batch_id() {
    // Reserve a thread-safe sequence value so concurrent builds cannot reuse an ID.
    const std::uint64_t sequence = next_sequence_.fetch_add(1);

    // Encode Agent, session, and sequence identity into a log-friendly stable string.
    std::ostringstream id;
    id << client_id_ << '-' << std::hex << std::setfill('0') << std::setw(16) << session_nonce_
       << '-' << std::dec << sequence;
    return id.str();
}

} // namespace pulsemesh
