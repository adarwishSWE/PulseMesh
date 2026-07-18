#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulsemesh {

struct AgentConfig {
    std::string client_id;
    std::chrono::milliseconds collection_interval{1000};
    std::vector<std::string> aggregator_endpoints;
    std::size_t queue_capacity = 256;
};

enum class AgentConfigErrorCode {
    kUnknownOption,
    kDuplicateOption,
    kMissingRequiredOption,
    kInvalidValue,
};

struct AgentConfigError {
    AgentConfigErrorCode code;
    std::string option;
};

// Parses --client-id, --interval-ms, repeatable --endpoint, and --queue-capacity options.
[[nodiscard]] std::expected<AgentConfig, AgentConfigError>
parse_agent_config(std::span<const std::string_view> arguments);

} // namespace pulsemesh
