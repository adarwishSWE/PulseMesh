#include "cpp/aggregator/impl/time_series_store.h"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <vector>

namespace pulsemesh {
namespace {

bool matches_tags(const Metric& metric,
    const google::protobuf::Map<std::string, std::string>& tags_filter) {
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
    AggregateStats stats;
    if (values.empty()) {
        return stats;
    }

    double sum = 0.0;
    stats.max_ = values.front();
    stats.min_ = values.front();
    for (const double value : values) {
        sum += value;
        stats.max_ = std::max(stats.max_, value);
        stats.min_ = std::min(stats.min_, value);
    }
    stats.avg_ = sum / static_cast<double>(values.size());
    return stats;
}

} // namespace

TimeSeriesStore::TimeSeriesStore(std::size_t capacity_per_metric)
    : capacity_per_metric_(
          capacity_per_metric == 0 ? kDefaultCapacityPerMetric : capacity_per_metric) {}

void TimeSeriesStore::Insert(const Metric& metric) {
    std::unique_lock lock(mutex_);

    std::deque<Metric>& buffer = metric_buffers_[metric.name()];
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

    if (buffer.size() > capacity_per_metric_) {
        buffer.pop_front();
    }
}

RangeResponse TimeSeriesStore::QueryRange(const RangeRequest& request) const {
    std::shared_lock lock(mutex_);

    RangeResponse response;
    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return response;
    }

    const std::deque<Metric>& buffer = buffer_it->second;
    const auto start_it = std::ranges::lower_bound(buffer,
        request.from_ms(),
        std::ranges::less{},
        [](const Metric& metric) {
            return metric.timestamp_ms();
        });

    std::vector<double> values;
    values.reserve(buffer.size());

    for (auto it = start_it; it != buffer.end() && it->timestamp_ms() <= request.to_ms(); ++it) {
        if (!matches_tags(*it, request.tags_filter())) {
            continue;
        }

        *response.add_metrics() = *it;
        values.push_back(it->value());
    }

    const AggregateStats stats = compute_stats(values);
    response.set_avg(stats.avg_);
    response.set_max(stats.max_);
    response.set_min(stats.min_);
    return response;
}

std::optional<LatestResponse> TimeSeriesStore::QueryLatest(const LatestRequest& request) const {
    std::shared_lock lock(mutex_);

    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return std::nullopt;
    }

    const std::deque<Metric>& buffer = buffer_it->second;
    for (const Metric& metric : std::ranges::reverse_view(buffer)) {
        if (!matches_tags(metric, request.tags_filter())) {
            continue;
        }

        LatestResponse response;
        *response.mutable_metric() = metric;
        return response;
    }

    return std::nullopt;
}

} // namespace pulsemesh
