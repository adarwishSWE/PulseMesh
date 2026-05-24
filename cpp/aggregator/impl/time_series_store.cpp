#include "cpp/aggregator/impl/time_series_store.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace pulsemesh {
namespace {

bool MatchesTags(const Metric& metric,
                 const google::protobuf::Map<std::string, std::string>& tags_filter) {
    for (const auto& [filter_key, filter_value] : tags_filter) {
        const auto it = metric.tags().find(filter_key);
        if (it == metric.tags().end() || it->second != filter_value) {
            return false;
        }
    }
    return true;
}

struct AggregateStats {
    double avg = 0.0;
    double max = 0.0;
    double min = 0.0;
};

AggregateStats ComputeStats(const std::vector<double>& values) {
    AggregateStats stats;
    if (values.empty()) {
        return stats;
    }

    double sum = 0.0;
    stats.max = values.front();
    stats.min = values.front();
    for (const double value : values) {
        sum += value;
        stats.max = std::max(stats.max, value);
        stats.min = std::min(stats.min, value);
    }
    stats.avg = sum / static_cast<double>(values.size());
    return stats;
}

}  // namespace

TimeSeriesStore::TimeSeriesStore(std::size_t capacity_per_metric)
    : capacity_per_metric_(capacity_per_metric == 0 ? kDefaultCapacityPerMetric
                                                    : capacity_per_metric) {}

void TimeSeriesStore::Insert(const Metric& metric) {
    std::unique_lock lock(mutex_);

    std::deque<Metric>& buffer = metric_buffers_[metric.name()];
    if (buffer.empty() || metric.timestamp_ms() >= buffer.back().timestamp_ms()) {
        buffer.push_back(metric);
    } else {
        const auto insert_it = std::upper_bound(
            buffer.begin(),
            buffer.end(),
            metric.timestamp_ms(),
            [](int64_t timestamp_ms, const Metric& existing) {
                return timestamp_ms < existing.timestamp_ms();
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
    const auto start_it = std::lower_bound(
        buffer.begin(),
        buffer.end(),
        request.from_ms(),
        [](const Metric& metric, int64_t timestamp_ms) {
            return metric.timestamp_ms() < timestamp_ms;
        });

    std::vector<double> values;
    values.reserve(buffer.size());

    for (auto it = start_it;
         it != buffer.end() && it->timestamp_ms() <= request.to_ms();
         ++it) {
        if (!MatchesTags(*it, request.tags_filter())) {
            continue;
        }

        *response.add_metrics() = *it;
        values.push_back(it->value());
    }

    const AggregateStats stats = ComputeStats(values);
    response.set_avg(stats.avg);
    response.set_max(stats.max);
    response.set_min(stats.min);
    return response;
}

std::optional<LatestResponse> TimeSeriesStore::QueryLatest(const LatestRequest& request) const {
    std::shared_lock lock(mutex_);

    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return std::nullopt;
    }

    const std::deque<Metric>& buffer = buffer_it->second;
    for (auto it = buffer.rbegin(); it != buffer.rend(); ++it) {
        if (!MatchesTags(*it, request.tags_filter())) {
            continue;
        }

        LatestResponse response;
        *response.mutable_metric() = *it;
        return response;
    }

    return std::nullopt;
}

}  // namespace pulsemesh
