#include "cpp/client/impl/agent_config/agent_config.h"

#include <charconv>

namespace pulsemesh {
namespace {

constexpr std::string_view kClientIdOption = "--client-id=";
constexpr std::string_view kIntervalOption = "--interval-ms=";
constexpr std::string_view kEndpointOption = "--endpoint=";
constexpr std::string_view kQueueCapacityOption = "--queue-capacity=";

AgentConfigError make_error(AgentConfigErrorCode code, std::string_view option) {
    // Own the option text so the error never depends on the argument array's lifetime.
    return AgentConfigError{.code = code, .option = std::string{option}};
}

template <typename Integer>
bool parse_positive_integer(std::string_view text, Integer& value) {
    // Reject absence before parsing so default-initialized zero cannot look valid.
    if (text.empty()) {
        return false;
    }

    // Accept only a complete, representable, strictly positive integer value.
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0;
}

struct ConfigParseState {
    AgentConfig config_;
    bool has_client_id_ = false;
    bool has_interval_ = false;
    bool has_queue_capacity_ = false;
};

std::expected<void, AgentConfigError> parse_client_id(ConfigParseState& state,
                                                      std::string_view value) {
    // Reject ambiguity and empty Agent identities before updating configuration state.
    if (state.has_client_id_) {
        return std::unexpected(make_error(AgentConfigErrorCode::kDuplicateOption, kClientIdOption));
    }
    if (value.empty()) {
        return std::unexpected(make_error(AgentConfigErrorCode::kInvalidValue, kClientIdOption));
    }

    // Store the validated identity and remember that the scalar option was consumed.
    state.config_.client_id = value;
    state.has_client_id_ = true;
    return {};
}

std::expected<void, AgentConfigError> parse_interval(ConfigParseState& state,
                                                     std::string_view value) {
    // Permit one interval override so command-line order cannot silently change the result.
    if (state.has_interval_) {
        return std::unexpected(make_error(AgentConfigErrorCode::kDuplicateOption, kIntervalOption));
    }
    std::chrono::milliseconds::rep interval_ms = 0;
    if (!parse_positive_integer(value, interval_ms)) {
        return std::unexpected(make_error(AgentConfigErrorCode::kInvalidValue, kIntervalOption));
    }

    // Store the validated count in a unit-safe duration.
    state.config_.collection_interval = std::chrono::milliseconds{interval_ms};
    state.has_interval_ = true;
    return {};
}

std::expected<void, AgentConfigError> parse_endpoint(ConfigParseState& state,
                                                     std::string_view value) {
    // Every repeated endpoint must name a destination; empty failover entries are invalid.
    if (value.empty()) {
        return std::unexpected(make_error(AgentConfigErrorCode::kInvalidValue, kEndpointOption));
    }

    // Preserve endpoint order for the future gRPC connection and failover policy.
    state.config_.aggregator_endpoints.emplace_back(value);
    return {};
}

std::expected<void, AgentConfigError> parse_queue_capacity(ConfigParseState& state,
                                                           std::string_view value) {
    // Permit one capacity override and require a non-zero bounded-queue size.
    if (state.has_queue_capacity_) {
        return std::unexpected(
            make_error(AgentConfigErrorCode::kDuplicateOption, kQueueCapacityOption));
    }
    std::size_t queue_capacity = 0;
    if (!parse_positive_integer(value, queue_capacity)) {
        return std::unexpected(
            make_error(AgentConfigErrorCode::kInvalidValue, kQueueCapacityOption));
    }

    // Store the validated capacity and prevent a later option from replacing it.
    state.config_.queue_capacity = queue_capacity;
    state.has_queue_capacity_ = true;
    return {};
}

std::expected<void, AgentConfigError> parse_argument(ConfigParseState& state,
                                                     std::string_view argument) {
    // Split one --option=value argument without allocating or modifying caller-owned text.
    const std::size_t separator = argument.find('=');
    if (separator == std::string_view::npos) {
        return std::unexpected(make_error(AgentConfigErrorCode::kUnknownOption, argument));
    }

    const std::string_view option = argument.substr(0, separator + 1);
    const std::string_view value = argument.substr(separator + 1);

    // Dispatch recognized options to their type-specific validation and mutation step.
    if (option == kClientIdOption) {
        return parse_client_id(state, value);
    }
    if (option == kIntervalOption) {
        return parse_interval(state, value);
    }
    if (option == kEndpointOption) {
        return parse_endpoint(state, value);
    }
    if (option == kQueueCapacityOption) {
        return parse_queue_capacity(state, value);
    }

    // Reject unknown options instead of silently accepting a misspelled configuration.
    return std::unexpected(make_error(AgentConfigErrorCode::kUnknownOption, option));
}

} // namespace

std::expected<AgentConfig, AgentConfigError>
parse_agent_config(std::span<const std::string_view> arguments) {
    // Parse each argument in order and stop at the first actionable configuration error.
    ConfigParseState state;
    for (const std::string_view argument : arguments) {
        if (const auto result = parse_argument(state, argument); !result) {
            return std::unexpected(result.error());
        }
    }

    // Enforce the identity and destination options that have no safe operational defaults.
    if (!state.has_client_id_) {
        return std::unexpected(
            make_error(AgentConfigErrorCode::kMissingRequiredOption, kClientIdOption));
    }
    if (state.config_.aggregator_endpoints.empty()) {
        return std::unexpected(
            make_error(AgentConfigErrorCode::kMissingRequiredOption, kEndpointOption));
    }

    // Publish the complete typed configuration only after every option and requirement passes.
    return state.config_;
}

} // namespace pulsemesh
