#include "cpp/aggregator/impl/in_memory_time_series_store/in_memory_time_series_store.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulsemesh {
namespace {

constexpr std::uint32_t kDefaultPageSize = 1000;
constexpr std::uint32_t kMaximumPageSize = 10000;
constexpr std::string_view kPageTokenPrefix = "v1:";

struct AggregateStats {
    double avg_ = 0.0;
    double max_ = 0.0;
    double min_ = 0.0;
};

struct RangePageCursor {
    std::int64_t snapshot_event_id_;
    std::int64_t timestamp_ms_;
    std::int64_t event_id_;
};

struct RangeQueryPlan {
    std::optional<RangePageCursor> cursor_;
    std::uint32_t page_size_;
};

TimeSeriesStoreError make_error(TimeSeriesStoreErrorCode code, std::string operation,
                                std::string context, bool retryable) {
    return TimeSeriesStoreError{
        .code = code,
        .operation = std::move(operation),
        .context = std::move(context),
        .retryable = retryable,
    };
}

bool matches_tags(const Metric& metric,
                  const google::protobuf::Map<std::string, std::string>& tags_filter) {
    // A metric matches only when every requested key exists with the requested value.
    return std::ranges::all_of(tags_filter, [&metric](const auto& filter_entry) {
        const auto tag_it = metric.tags().find(filter_entry.first);
        return tag_it != metric.tags().end() && tag_it->second == filter_entry.second;
    });
}

template <typename Request>
bool matches_filters(const Metric& metric, const Request& request) {
    // Apply the common metric name, optional server-controlled client identity, and tag filters.
    return metric.name() == request.metric_name() &&
        (!request.has_client_id() || metric.source_client_id() == request.client_id()) &&
        matches_tags(metric, request.tags_filter());
}

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

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

std::optional<std::string_view> take_token_field(std::string_view& token) {
    const std::size_t separator = token.find(':');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view field = token.substr(0, separator);
    token.remove_prefix(separator + 1);
    return field;
}

std::optional<RangePageCursor> decode_page_token(std::string_view token) {
    // Require the supported version before interpreting the three numeric cursor fields.
    if (!token.starts_with(kPageTokenPrefix)) {
        return std::nullopt;
    }
    token.remove_prefix(kPageTokenPrefix.size());

    const auto snapshot_field = take_token_field(token);
    const auto timestamp_field = take_token_field(token);
    if (!snapshot_field || !timestamp_field) {
        return std::nullopt;
    }

    // Parse the snapshot boundary and last emitted ordering key without accepting trailing text.
    RangePageCursor cursor{};
    if (!parse_integer(*snapshot_field, cursor.snapshot_event_id_) ||
        !parse_integer(*timestamp_field, cursor.timestamp_ms_) ||
        !parse_integer(token, cursor.event_id_) || cursor.snapshot_event_id_ <= 0 ||
        cursor.event_id_ <= 0 || cursor.event_id_ > cursor.snapshot_event_id_) {
        return std::nullopt;
    }
    return cursor;
}

std::string encode_page_token(RangePageCursor cursor) {
    // Keep the token opaque to callers while retaining a version for future compatible decoding.
    return std::string{kPageTokenPrefix} + std::to_string(cursor.snapshot_event_id_) + ':' +
        std::to_string(cursor.timestamp_ms_) + ':' + std::to_string(cursor.event_id_);
}

std::optional<TimeSeriesStoreError> validate_batch(const MetricBatch& batch) {
    // Validate all externally supplied identity and payload fields before acquiring the write lock.
    if (batch.client_id().empty()) {
        return make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                          "commit_batch",
                          "client_id",
                          false);
    }
    if (batch.batch_id().empty()) {
        return make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                          "commit_batch",
                          "batch_id",
                          false);
    }
    if (batch.metrics().empty()) {
        return make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                          "commit_batch",
                          "metrics",
                          false);
    }
    for (const Metric& metric : batch.metrics()) {
        if (metric.name().empty() || !std::isfinite(metric.value())) {
            return make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                              "commit_batch",
                              "metric",
                              false);
        }
    }
    return std::nullopt;
}

template <typename Request>
std::optional<TimeSeriesStoreError> validate_query_filters(const Request& request,
                                                           std::string operation) {
    // Reject filters that cannot identify a metric or contain an explicitly empty client ID.
    if (request.metric_name().empty()) {
        return make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                          std::move(operation),
                          "metric_name",
                          false);
    }
    if (request.has_client_id() && request.client_id().empty()) {
        return make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                          std::move(operation),
                          "client_id",
                          false);
    }
    return std::nullopt;
}

bool is_after_cursor(std::int64_t timestamp_ms, std::int64_t event_id,
                     const RangePageCursor& cursor) {
    return timestamp_ms > cursor.timestamp_ms_ ||
        (timestamp_ms == cursor.timestamp_ms_ && event_id > cursor.event_id_);
}

std::expected<RangeQueryPlan, TimeSeriesStoreError>
make_range_query_plan(const RangeRequest& request) {
    // Validate common filters and the inclusive time interval before interpreting pagination.
    if (const auto error = validate_query_filters(request, "query_range")) {
        return std::unexpected(*error);
    }
    if (request.from_ms() > request.to_ms()) {
        return std::unexpected(make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                                          "query_range",
                                          "time_range",
                                          false));
    }
    if (request.page_size() > kMaximumPageSize) {
        return std::unexpected(make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                                          "query_range",
                                          "page_size",
                                          false));
    }

    // Decode an optional continuation and normalize zero to the server-side page default.
    std::optional<RangePageCursor> cursor;
    if (!request.page_token().empty()) {
        cursor = decode_page_token(request.page_token());
        if (!cursor) {
            return std::unexpected(make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                                              "query_range",
                                              "page_token",
                                              false));
        }
    }
    return RangeQueryPlan{
        .cursor_ = cursor,
        .page_size_ = request.page_size() == 0 ? kDefaultPageSize : request.page_size(),
    };
}

template <typename Buffer, typename Stored>
void insert_bounded_metric(Buffer& buffer, Stored stored, std::size_t capacity) {
    // Preserve timestamp order and use event ID as the deterministic equal-timestamp tie-break.
    const auto stored_key = std::pair{stored.metric_.timestamp_ms(), stored.event_id_};
    const auto key_of = [](const auto& existing) {
        return std::pair{existing.metric_.timestamp_ms(), existing.event_id_};
    };
    if (buffer.empty() || stored_key >= key_of(buffer.back())) {
        buffer.push_back(std::move(stored));
    } else {
        const auto insert_it =
            std::ranges::upper_bound(buffer, stored_key, std::ranges::less{}, key_of);
        buffer.insert(insert_it, std::move(stored));
    }

    // Evict the oldest timestamp when this metric exceeds its configured retention bound.
    if (buffer.size() > capacity) {
        buffer.pop_front();
    }
}

template <typename BufferMap>
void publish_staged_buffers(BufferMap& destination, BufferMap staged) {
    // Swap existing buffers and transfer prepared nodes for metric names not seen before.
    for (auto staged_it = staged.begin(); staged_it != staged.end();) {
        auto current_it = staged_it++;
        const auto destination_it = destination.find(current_it->first);
        if (destination_it != destination.end()) {
            destination_it->second.swap(current_it->second);
            continue;
        }

        auto node = staged.extract(current_it);
        destination.insert(std::move(node));
    }
}

template <typename Buffer>
RangeResponse build_range_page(const Buffer& buffer, const RangeRequest& request,
                               const RangeQueryPlan& plan, std::int64_t snapshot_event_id) {
    // Seek directly to the first timestamp that can satisfy the inclusive range.
    const auto start_it = std::ranges::lower_bound(buffer,
                                                   request.from_ms(),
                                                   std::ranges::less{},
                                                   [](const auto& stored) {
                                                       return stored.metric_.timestamp_ms();
                                                   });

    RangeResponse response;
    std::vector<double> values;
    values.reserve(buffer.size());
    std::optional<RangePageCursor> last_returned;
    bool has_more = false;

    // Compute aggregates over the complete filtered snapshot while copying one requested page.
    for (auto it = start_it; it != buffer.end() && it->metric_.timestamp_ms() <= request.to_ms();
         ++it) {
        if (it->event_id_ > snapshot_event_id || !matches_filters(it->metric_, request)) {
            continue;
        }
        values.push_back(it->metric_.value());

        if (plan.cursor_ &&
            !is_after_cursor(it->metric_.timestamp_ms(), it->event_id_, *plan.cursor_)) {
            continue;
        }
        if (static_cast<std::uint32_t>(response.metrics_size()) == plan.page_size_) {
            has_more = true;
            continue;
        }

        *response.add_metrics() = it->metric_;
        last_returned = RangePageCursor{
            .snapshot_event_id_ = snapshot_event_id,
            .timestamp_ms_ = it->metric_.timestamp_ms(),
            .event_id_ = it->event_id_,
        };
    }

    // Attach full-range aggregates and a continuation only when another matching sample exists.
    const AggregateStats stats = compute_stats(values);
    response.set_avg(stats.avg_);
    response.set_max(stats.max_);
    response.set_min(stats.min_);
    if (has_more && last_returned) {
        response.set_next_page_token(encode_page_token(*last_returned));
    }
    return response;
}

} // namespace

InMemoryTimeSeriesStore::InMemoryTimeSeriesStore(std::size_t capacity_per_metric)
    : capacity_per_metric_(capacity_per_metric == 0 ? kDefaultCapacityPerMetric
                                                    : capacity_per_metric) {}

std::expected<BatchCommitOutcome, TimeSeriesStoreError>
InMemoryTimeSeriesStore::commit_batch(const MetricBatch& batch) {
    // Validate the complete batch before any deduplication or storage state can change.
    if (const auto error = validate_batch(batch)) {
        return std::unexpected(*error);
    }

    // Serialize duplicate detection, event assignment, and publication as one state transition.
    std::unique_lock lock(mutex_);
    if (committed_batch_ids_.contains(batch.batch_id())) {
        return BatchCommitOutcome::kDuplicate;
    }

    const auto metric_count = static_cast<std::int64_t>(batch.metrics_size());
    if (metric_count > std::numeric_limits<std::int64_t>::max() - current_event_id_) {
        return std::unexpected(make_error(TimeSeriesStoreErrorCode::kConstraint,
                                          "commit_batch",
                                          "event_id_exhausted",
                                          false));
    }

    // Copy only affected buffers so readers observe either the old state or the complete batch.
    std::unordered_map<std::string, std::deque<StoredMetric>> staged_buffers;
    staged_buffers.reserve(static_cast<std::size_t>(batch.metrics_size()));
    std::int64_t next_event_id = current_event_id_;
    for (const Metric& metric : batch.metrics()) {
        auto [staged_it, inserted] = staged_buffers.try_emplace(metric.name());
        if (inserted) {
            const auto current_it = metric_buffers_.find(metric.name());
            if (current_it != metric_buffers_.end()) {
                staged_it->second = current_it->second;
            }
        }

        // Replace any Agent-supplied source identity and assign commit order inside this batch.
        Metric stored_metric = metric;
        stored_metric.set_source_client_id(batch.client_id());
        StoredMetric stored{
            .metric_ = std::move(stored_metric),
            .event_id_ = ++next_event_id,
        };
        insert_bounded_metric(staged_it->second, std::move(stored), capacity_per_metric_);
    }

    // Publish the staged buffers only after every metric has been prepared successfully.
    metric_buffers_.reserve(metric_buffers_.size() + staged_buffers.size());
    // ponytail: The development backend retains batch IDs for its process lifetime. Replace this
    // set with the PostgreSQL unique constraint when durable scale or restart survival is required.
    committed_batch_ids_.insert(batch.batch_id());
    publish_staged_buffers(metric_buffers_, std::move(staged_buffers));
    current_event_id_ = next_event_id;
    return BatchCommitOutcome::kCommitted;
}

std::expected<RangeResponse, TimeSeriesStoreError>
InMemoryTimeSeriesStore::query_range(const RangeRequest& request) const {
    // Validate and normalize the complete request before reading shared state.
    const auto plan = make_range_query_plan(request);
    if (!plan) {
        return std::unexpected(plan.error());
    }

    // Hold one shared lock so the selected snapshot and all returned aggregates are coherent.
    std::shared_lock lock(mutex_);
    const RangePageCursor cursor = plan->cursor_.value_or(RangePageCursor{
        .snapshot_event_id_ = current_event_id_,
        .timestamp_ms_ = 0,
        .event_id_ = 0,
    });
    if (cursor.snapshot_event_id_ > current_event_id_) {
        return std::unexpected(make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                                          "query_range",
                                          "page_token",
                                          false));
    }

    // Return neutral aggregates when the requested metric has no retained samples.
    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return RangeResponse{};
    }

    // Build one page and its full-snapshot aggregates while the selected buffer remains stable.
    return build_range_page(buffer_it->second, request, *plan, cursor.snapshot_event_id_);
}

std::expected<std::optional<LatestResponse>, TimeSeriesStoreError>
InMemoryTimeSeriesStore::query_latest(const LatestRequest& request) const {
    // Validate filters before reading shared state.
    if (const auto error = validate_query_filters(request, "query_latest")) {
        return std::unexpected(*error);
    }

    // Hold a shared lock while locating and scanning the requested metric buffer.
    std::shared_lock lock(mutex_);
    const auto buffer_it = metric_buffers_.find(request.metric_name());
    if (buffer_it == metric_buffers_.end()) {
        return std::optional<LatestResponse>{};
    }

    // Scan newest-to-oldest and return the first sample satisfying every filter.
    for (const StoredMetric& stored : std::ranges::reverse_view(buffer_it->second)) {
        if (!matches_filters(stored.metric_, request)) {
            continue;
        }

        LatestResponse response;
        *response.mutable_metric() = stored.metric_;
        return std::optional<LatestResponse>{std::move(response)};
    }
    return std::optional<LatestResponse>{};
}

std::expected<std::int64_t, TimeSeriesStoreError>
InMemoryTimeSeriesStore::current_event_id() const {
    // Read the compound commit state under the same lock that publishes batches.
    std::shared_lock lock(mutex_);
    return current_event_id_;
}

std::expected<std::vector<MetricEvent>, TimeSeriesStoreError>
InMemoryTimeSeriesStore::query_events_after(const SubscribeRequest& request,
                                            std::uint32_t fetch_limit) const {
    // Validate filters, the exclusive cursor, and the caller's explicit work bound.
    if (const auto error = validate_query_filters(request, "query_events_after")) {
        return std::unexpected(*error);
    }
    if (request.after_event_id() < 0) {
        return std::unexpected(make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                                          "query_events_after",
                                          "after_event_id",
                                          false));
    }
    if (fetch_limit == 0 || fetch_limit > kMaximumPageSize) {
        return std::unexpected(make_error(TimeSeriesStoreErrorCode::kInvalidArgument,
                                          "query_events_after",
                                          "fetch_limit",
                                          false));
    }

    // Scan retained development data and select matching events after the exclusive cursor.
    std::shared_lock lock(mutex_);
    // ponytail: This is O(total retained samples) and intended only for the bounded in-memory
    // backend. PostgreSQL replaces it with an indexed event_id query in the production store.
    std::vector<const StoredMetric*> matches;
    for (const auto& metric_buffer : metric_buffers_) {
        for (const StoredMetric& stored : metric_buffer.second) {
            if (stored.event_id_ > request.after_event_id() &&
                matches_filters(stored.metric_, request)) {
                matches.push_back(&stored);
            }
        }
    }

    // Restore global commit order across independently timestamp-sorted metric buffers.
    std::ranges::sort(matches, std::ranges::less{}, [](const StoredMetric* stored) {
        return stored->event_id_;
    });
    if (matches.size() > fetch_limit) {
        matches.resize(fetch_limit);
    }

    // Materialize owning wire events before releasing the store lock.
    std::vector<MetricEvent> events;
    events.reserve(matches.size());
    for (const StoredMetric* stored : matches) {
        MetricEvent event;
        *event.mutable_metric() = stored->metric_;
        event.set_event_id(stored->event_id_);
        events.push_back(std::move(event));
    }
    return events;
}

} // namespace pulsemesh
