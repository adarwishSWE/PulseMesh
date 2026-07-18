#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <vector>

#include "proto/metrics.pb.h"

namespace pulsemesh {

enum class MetricCollectionErrorCode {
    kReadFailed,
    kMalformedSource,
    kInvalidCounterDelta,
};

struct MetricCollectionError {
    MetricCollectionErrorCode code;
    std::string operation;
    std::string source;
    bool retryable;
};

class IMetricCollector {
public:
    IMetricCollector() = default;
    virtual ~IMetricCollector() = default;

    [[nodiscard]] virtual std::expected<std::vector<Metric>, MetricCollectionError>
    collect(std::chrono::system_clock::time_point sampled_at) = 0;

    IMetricCollector(const IMetricCollector&) = delete;
    IMetricCollector& operator=(const IMetricCollector&) = delete;
    IMetricCollector(IMetricCollector&&) = delete;
    IMetricCollector& operator=(IMetricCollector&&) = delete;
};

} // namespace pulsemesh
