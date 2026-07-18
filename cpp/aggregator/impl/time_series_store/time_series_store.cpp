#include "cpp/aggregator/impl/time_series_store/time_series_store.h"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <vector>

namespace pulsemesh {
namespace {

bool matches_tags(const Metric& metric,
                  const google::protobuf::Map<std::string, std::string>& tags_filter) {
    // A metric matches only when every requested key exists with the requested value.
    return std::ranges::all_of(tags_filter, [&metric](const auto& filter_entry) {
        const auto tag_it = metric.tags().find(filter_entry.first);
        return tag_it != metric.tags().end() && tag_it->second == filter_entry.second;
    });
}

struct AggregateStats {
    double avg_ = 0.0;
    double max_ = 0.0;
    double min_ = 0.0;
};

AggregateStats compute_stats(const std::vector<double>& values) {
    // Empty ranges use the Protobuf response's neutral aggregate values.
    AggregateStats stats;
    if (values.empty()) {
        return stats;
    }

    // Compute all aggregates in one pass over the already-filtered values.
    double sum = 0.0;
    stats.max_ = values.front();
    stats.min_ = values.front();
    for (const double value : values) {
        sum += value;
        stats.max_ = std::max(stats.max_, value);
        stats.min_ = std::min(stats.min_, value);
    }

    // Finalize the average only after the full filtered range is known.
    stats.avg_ = sum / static_cast<double>(values.size());
    return stats;
}

} // namespace

TimeSeriesStore::TimeSeriesStore(std::size_t capacity_per_metric)
    : capacity_per_metric_(capacity_per_metric == 0 ? kDefaultCapacityPerMetric
                                                    : capacity_per_metric) {}

void TimeSeriesStore::Insert(const Metric& metric) {
    // Select the bounded time-ordered buffer owned by this metric name.
    std::unique_lock lock(mutex_);

    std::deque<Metric>& buffer = metric_buffers_[metric.name()];
    // Preserve timestamp ordering while keeping the common in-order insertion path O(1).
    if (buffer.empty() || metric.timestamp_ms() >= buffer.back().timestamp_ms()) {
        buffer.push_back(metric);
    } else {
        const auto insert_it = std::ranges::upper_bound(buffer,
                                                        metric.timestamp_ms(),
                                                        std::ranges::less{},
                                                        [](const Metric& existing) {
                                                            return existing.timestamp_ms();
                                                        });
        buffer.insert(insert_it, metric);
    }

    // Evict the oldest sample when this metric exceeds its configured retention bound.
    if (buffer.size() > capacity_per_metric_) {
        buffer.pop_front();
    }
}

RangeResponse TimeSeriesStore::QueryRange(const RangeRequest& request) const {
    // Hold a shared lock while reading the selected metric's ordered buffer.
    std::shared_lock lock(mutex_);

    // Return an empty response when the requested metric has never been collected.
    RangeResponse response;
    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return response;
    }

    // Seek directly to the first timestamp that can satisfy the inclusive range.
    const std::deque<Metric>& buffer = buffer_it->second;
    const auto start_it = std::ranges::lower_bound(buffer,
                                                   request.from_ms(),
                                                   std::ranges::less{},
                                                   [](const Metric& metric) {
                                                       return metric.timestamp_ms();
                                                   });

    std::vector<double> values;
    values.reserve(buffer.size());

    // Copy matching samples and collect their values for full-range aggregates.
    for (auto it = start_it; it != buffer.end() && it->timestamp_ms() <= request.to_ms(); ++it) {
        if (!matches_tags(*it, request.tags_filter())) {
            continue;
        }

        *response.add_metrics() = *it;
        values.push_back(it->value());
    }

    // Attach aggregates computed over the same filtered samples returned above.
    const AggregateStats stats = compute_stats(values);
    response.set_avg(stats.avg_);
    response.set_max(stats.max_);
    response.set_min(stats.min_);
    return response;
}

std::optional<LatestResponse> TimeSeriesStore::QueryLatest(const LatestRequest& request) const {
    // Hold a shared lock while locating the requested metric buffer.
    std::shared_lock lock(mutex_);

    // Absence is a normal result when the metric name has no retained samples.
    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return std::nullopt;
    }

    // Scan newest-to-oldest and return the first sample satisfying every tag filter.
    const std::deque<Metric>& buffer = buffer_it->second;
    for (const Metric& metric : std::ranges::reverse_view(buffer)) {
        if (!matches_tags(metric, request.tags_filter())) {
            continue;
        }

        LatestResponse response;
        *response.mutable_metric() = metric;
        return response;
    }

    // The metric exists, but none of its retained samples satisfy the requested tags.
    return std::nullopt;
}

} // namespace pulsemesh
