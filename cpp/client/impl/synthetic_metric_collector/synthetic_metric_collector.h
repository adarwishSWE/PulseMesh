#pragma once

#include <cstdint>
#include <string>

#include "cpp/client/i_metric_collector.h"

namespace pulsemesh {

struct SyntheticMetricConfig {
    std::string metric_name = "synthetic_value";
    double initial_value = 0.0;
    double step = 1.0;
};

// Single-threaded deterministic collector for tests and load generation.
class SyntheticMetricCollector final : public IMetricCollector {
public:
    explicit SyntheticMetricCollector(SyntheticMetricConfig config = {});
    ~SyntheticMetricCollector() override = default;

    [[nodiscard]] std::expected<std::vector<Metric>, MetricCollectionError>
    collect(std::chrono::system_clock::time_point sampled_at) override;

    SyntheticMetricCollector(const SyntheticMetricCollector&) = delete;
    SyntheticMetricCollector& operator=(const SyntheticMetricCollector&) = delete;
    SyntheticMetricCollector(SyntheticMetricCollector&&) = delete;
    SyntheticMetricCollector& operator=(SyntheticMetricCollector&&) = delete;

private:
    std::string metric_name_;
    double initial_value_;
    double step_;
    std::uint64_t sample_index_ = 0;
};

} // namespace pulsemesh
