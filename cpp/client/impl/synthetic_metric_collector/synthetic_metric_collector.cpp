#include "cpp/client/impl/synthetic_metric_collector/synthetic_metric_collector.h"

#include <chrono>
#include <cmath>
#include <utility>

namespace pulsemesh {

SyntheticMetricCollector::SyntheticMetricCollector(SyntheticMetricConfig config)
    : metric_name_(std::move(config.metric_name)), initial_value_(config.initial_value),
      step_(config.step) {}

std::expected<std::vector<Metric>, MetricCollectionError>
SyntheticMetricCollector::collect(std::chrono::system_clock::time_point sampled_at) {
    // Validate immutable generator configuration before producing a sample.
    if (metric_name_.empty() || !std::isfinite(initial_value_) || !std::isfinite(step_)) {
        return std::unexpected(MetricCollectionError{
            .code = MetricCollectionErrorCode::kMalformedSource,
            .operation = "generate_synthetic_metric",
            .source = "synthetic",
            .retryable = false,
        });
    }

    // Derive each sample from the original value and integer index so rounding from one sample
    // never becomes input to the next sample.
    const double value = initial_value_ + (static_cast<double>(sample_index_) * step_);
    if (!std::isfinite(value)) {
        return std::unexpected(MetricCollectionError{
            .code = MetricCollectionErrorCode::kMalformedSource,
            .operation = "generate_synthetic_metric",
            .source = "synthetic",
            .retryable = false,
        });
    }

    // Materialize one deterministic metric at the caller-provided sampling time.
    Metric metric;
    metric.set_name(metric_name_);
    metric.set_value(value);
    metric.set_timestamp_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(sampled_at.time_since_epoch())
            .count());

    // Advance only after successful materialization so failed calls do not skip sequence values.
    ++sample_index_;

    // Return the common collector result shape used by real and synthetic collectors.
    std::vector<Metric> metrics;
    metrics.push_back(std::move(metric));
    return metrics;
}

} // namespace pulsemesh
