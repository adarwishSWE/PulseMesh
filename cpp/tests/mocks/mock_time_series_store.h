#pragma once

#include <optional>

#include <gmock/gmock.h>

#include "cpp/aggregator/i_time_series_store.h"

namespace pulsemesh {

class MockTimeSeriesStore : public ITimeSeriesStore {
public:
    MOCK_METHOD(void, Insert, (const Metric& metric), (override));
    MOCK_METHOD(RangeResponse, QueryRange, (const RangeRequest& request), (const, override));
    MOCK_METHOD((std::optional<LatestResponse>), QueryLatest, (const LatestRequest& request),
        (const, override));
};

} // namespace pulsemesh
