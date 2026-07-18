// Tests BatchBuilder validation, payload construction, and stable unique batch IDs.

#include "cpp/client/impl/batch_builder/batch_builder.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace pulsemesh {
namespace {

Metric make_metric(std::string name, double value) {
    Metric metric;
    metric.set_name(std::move(name));
    metric.set_value(value);
    return metric;
}

} // namespace

class BatchBuilderTest : public ::testing::Test {};

// Verifies that valid metrics receive distinct stable batch identities while the Agent identity
// and metric payload are preserved in the completed wire batch.
TEST_F(BatchBuilderTest, GivenValidMetrics_WhenBuilt_ThenCreatesDistinctIdentifiedBatches) {
    // Given
    BatchBuilder builder{"agent-a", 0x1234};

    // When
    auto first = builder.build({make_metric("cpu", 10.0)});
    auto second = builder.build({make_metric("cpu", 20.0)});

    // Then
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->client_id(), "agent-a");
    EXPECT_EQ(first->metrics_size(), 1);
    EXPECT_EQ(first->metrics(0).value(), 10.0);
    EXPECT_FALSE(first->batch_id().empty());
    EXPECT_NE(first->batch_id(), second->batch_id());
}

// Verifies that non-finite metric values are classified as invalid before a batch ID is assigned
// or malformed data can enter the pending queue.
TEST_F(BatchBuilderTest, GivenNonFiniteMetric_WhenBuilt_ThenReturnsInvalidMetric) {
    // Given
    BatchBuilder builder{"agent-a", 0x1234};

    // When
    const auto result =
        builder.build({make_metric("cpu", std::numeric_limits<double>::quiet_NaN())});

    // Then
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, BatchBuildErrorCode::kInvalidMetric);
}

} // namespace pulsemesh
