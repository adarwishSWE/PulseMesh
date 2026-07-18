// Tests Agent command-line configuration defaults, repeated endpoints, and validation.

#include "cpp/client/impl/agent_config/agent_config.h"

#include <array>
#include <chrono>
#include <string_view>

#include <gtest/gtest.h>

namespace pulsemesh {

class AgentConfigTest : public ::testing::Test {};

// Verifies that every supported CLI option is converted into its typed configuration field and
// that repeated endpoints preserve their command-line order for future failover.
TEST_F(AgentConfigTest, GivenCompleteArguments_WhenParsed_ThenBuildsTypedConfiguration) {
    // Given
    constexpr std::array arguments{
        std::string_view{"--client-id=agent-a"},
        std::string_view{"--interval-ms=250"},
        std::string_view{"--endpoint=aggregator-a:50051"},
        std::string_view{"--endpoint=aggregator-b:50051"},
        std::string_view{"--queue-capacity=512"},
    };

    // When
    const auto result = parse_agent_config(arguments);

    // Then
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->client_id, "agent-a");
    EXPECT_EQ(result->collection_interval, std::chrono::milliseconds{250});
    ASSERT_EQ(result->aggregator_endpoints.size(), 2);
    EXPECT_EQ(result->aggregator_endpoints.at(1), "aggregator-b:50051");
    EXPECT_EQ(result->queue_capacity, 512);
}

// Verifies that optional interval and capacity arguments use the approved bounded defaults when
// the required Agent identity and Aggregator endpoint are present.
TEST_F(AgentConfigTest, GivenRequiredArguments_WhenParsed_ThenUsesBoundedDefaults) {
    // Given
    constexpr std::array arguments{
        std::string_view{"--client-id=agent-a"},
        std::string_view{"--endpoint=aggregator:50051"},
    };

    // When
    const auto result = parse_agent_config(arguments);

    // Then
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->collection_interval, std::chrono::milliseconds{1000});
    EXPECT_EQ(result->queue_capacity, 256);
}

// Verifies that a zero queue capacity is rejected as a typed value error rather than silently
// creating an unbounded or unusable pending queue.
TEST_F(AgentConfigTest, GivenInvalidCapacity_WhenParsed_ThenReturnsTypedError) {
    // Given
    constexpr std::array arguments{
        std::string_view{"--client-id=agent-a"},
        std::string_view{"--endpoint=aggregator:50051"},
        std::string_view{"--queue-capacity=0"},
    };

    // When
    const auto result = parse_agent_config(arguments);

    // Then
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, AgentConfigErrorCode::kInvalidValue);
    EXPECT_EQ(result.error().option, "--queue-capacity=");
}

} // namespace pulsemesh
