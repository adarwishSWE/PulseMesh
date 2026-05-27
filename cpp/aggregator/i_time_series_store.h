#pragma once

#include <optional>

#include "proto/metrics.pb.h"

namespace pulsemesh {

class ITimeSeriesStore {
public:
    ITimeSeriesStore() = default;
    virtual ~ITimeSeriesStore() = default;

    virtual void Insert(const Metric& metric) = 0;

    virtual RangeResponse QueryRange(const RangeRequest& request) const = 0;

    // Returns std::nullopt when no metric matches the request.
    virtual std::optional<LatestResponse> QueryLatest(const LatestRequest& request) const = 0;

    ITimeSeriesStore(const ITimeSeriesStore&) = delete;
    ITimeSeriesStore& operator=(const ITimeSeriesStore&) = delete;
    ITimeSeriesStore(ITimeSeriesStore&&) = delete;
    ITimeSeriesStore& operator=(ITimeSeriesStore&&) = delete;
};

} // namespace pulsemesh
