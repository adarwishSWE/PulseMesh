#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "proto/metrics.pb.h"

namespace pulsemesh {

enum class BatchBuildErrorCode {
    kEmptyClientId,
    kEmptyMetrics,
    kInvalidMetric,
};

struct BatchBuildError {
    BatchBuildErrorCode code;
};

// Thread-safe generator of process-unique batch IDs and immutable batch payloads.
class BatchBuilder {
public:
    explicit BatchBuilder(std::string client_id);
    BatchBuilder(std::string client_id, std::uint64_t session_nonce);

    [[nodiscard]] std::expected<MetricBatch, BatchBuildError> build(std::vector<Metric> metrics);

    BatchBuilder(const BatchBuilder&) = delete;
    BatchBuilder& operator=(const BatchBuilder&) = delete;
    BatchBuilder(BatchBuilder&&) = delete;
    BatchBuilder& operator=(BatchBuilder&&) = delete;

private:
    static std::uint64_t make_session_nonce();
    std::string next_batch_id();

    std::string client_id_;
    std::uint64_t session_nonce_;
    std::atomic<std::uint64_t> next_sequence_{1};
};

} // namespace pulsemesh
