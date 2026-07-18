// Tests deterministic SyntheticMetricCollector values, timestamps, and validation.

#include "cpp/client/impl/synthetic_metric_collector/synthetic_metric_collector.h"

#include <chrono>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace pulsemesh {

class SyntheticMetricCollectorTest : public ::testing::Test {};

// Verifies deterministic generation across consecutive samples, including configured name,
// initial value, step, and preservation of each supplied sampling timestamp.
TEST_F(SyntheticMetricCollectorTest, GivenConfiguredSequence_WhenCollected_ThenValuesAdvance) {
    // Given
    SyntheticMetricCollector collector{SyntheticMetricConfig{
        .metric_name = "requests",
        .initial_value = 10.0,
        .step = 2.5,
    }};

    // When
    const auto first =
        collector.collect(std::chrono::system_clock::time_point{std::chrono::milliseconds{1000}});
    const auto second =
        collector.collect(std::chrono::system_clock::time_point{std::chrono::milliseconds{2000}});

    // Then
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first->size(), 1);
    ASSERT_EQ(second->size(), 1);
    EXPECT_EQ(first->front().name(), "requests");
    EXPECT_DOUBLE_EQ(first->front().value(), 10.0);
    EXPECT_EQ(first->front().timestamp_ms(), 1000);
    EXPECT_DOUBLE_EQ(second->front().value(), 12.5);
    EXPECT_EQ(second->front().timestamp_ms(), 2000);
}

// Verifies that fractional sequences derive each value from the original configuration so
// rounding from earlier additions cannot accumulate over a long-running collection session.
TEST_F(SyntheticMetricCollectorTest,
       GivenFractionalStep_WhenManySamplesCollected_ThenRoundingDoesNotAccumulate) {
    // Given
    SyntheticMetricCollector collector{SyntheticMetricConfig{
        .initial_value = 0.0,
        .step = 0.1,
    }};
    const std::uint64_t last_sample_index = 1000;
    double last_value = 0.0;

    // When
    for (std::uint64_t sample_index = 0; sample_index <= last_sample_index; ++sample_index) {
        const auto result = collector.collect(std::chrono::system_clock::time_point{});
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->size(), 1);
        last_value = result->front().value();
    }

    // Then
    EXPECT_DOUBLE_EQ(last_value, static_cast<double>(last_sample_index) * 0.1);
}

// Verifies that non-finite generator state is rejected with a typed collection error before an
// invalid synthetic Metric can be published.
TEST_F(SyntheticMetricCollectorTest, GivenNonFiniteValue_WhenCollected_ThenReturnsTypedError) {
    // Given
    SyntheticMetricCollector collector{SyntheticMetricConfig{
        .metric_name = "invalid",
        .initial_value = std::numeric_limits<double>::infinity(),
    }};

    // When
    const auto result = collector.collect(std::chrono::system_clock::time_point{});

    // Then
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, MetricCollectionErrorCode::kMalformedSource);
}

} // namespace pulsemesh
