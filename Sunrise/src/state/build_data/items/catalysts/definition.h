#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::state::build_data::items::catalysts {

/** The target build contains fewer than 128 exotic weapon catalyst-shaped sockets. */
inline constexpr std::size_t kDefinitionCapacity = 128;
/** All bits set mark a catalyst relation with no completion-value requirement. */
inline constexpr std::uint16_t kUnavailableCompletionValueIndex = 0xFFFFU;
/** All bits set mark a catalyst relation with no completion-flag requirement. */
inline constexpr std::uint16_t kUnavailableCompletionFlagIndex = 0xFFFFU;
/** All bits set mark a catalyst relation with no acquired-state gate. */
inline constexpr std::uint16_t kUnavailableAcquisitionIndex = 0xFFFFU;

/** Release status from the build-scoped Season 11 availability data. */
enum class Availability : std::uint8_t {
    released = 0,
    placeholder = 1,
    unsupported = 2,
};

/** Safe catalog and resolver outcomes. */
enum class Error : std::uint8_t {
    none = 0,
    noCatalyst,
    placeholderOnly,
    unsupportedBuild,
    missingReleased,
    ambiguousLifecycle,
    invalidSocket,
    invalidPlug,
    invalidEffect,
    invalidAcquisition,
    invalidCompletion,
    invalidItemState,
};

/**
 * @param error Catalog or item-resolution result.
 * @return Stable log name for the result.
 */
[[nodiscard]] constexpr std::string_view error_name(Error error) noexcept {
    switch (error) {
    case Error::none:
        return "none";
    case Error::noCatalyst:
        return "no_catalyst";
    case Error::placeholderOnly:
        return "placeholder_only";
    case Error::unsupportedBuild:
        return "unsupported_build";
    case Error::missingReleased:
        return "missing_released";
    case Error::ambiguousLifecycle:
        return "ambiguous_lifecycle";
    case Error::invalidSocket:
        return "invalid_socket";
    case Error::invalidPlug:
        return "invalid_plug";
    case Error::invalidEffect:
        return "invalid_effect";
    case Error::invalidAcquisition:
        return "invalid_acquisition";
    case Error::invalidCompletion:
        return "invalid_completion";
    case Error::invalidItemState:
        return "invalid_item_state";
    }
    return "unknown";
}

/** One build-derived exotic weapon catalyst relation. */
struct Definition {
    std::uint32_t itemDefinitionHash{};
    std::uint16_t itemDefinitionIndex{};
    /** Unsupported rows use the unavailable item-index value. */
    std::uint16_t completedPlugDefinitionIndex{};
    /** Item row that supplies the completed catalyst's native perks and stat changes. */
    std::uint16_t effectDefinitionIndex{};
    /** Family-5 acquired-state slot that makes the catalyst socket visible. */
    std::uint16_t acquisitionDefinitionIndex{kUnavailableAcquisitionIndex};
    /** Family-5 flag slot used by later catalyst completion expressions. */
    std::uint16_t completionFlagDefinitionIndex{kUnavailableCompletionFlagIndex};
    /** Family-5 value slot used by legacy catalyst completion expressions. */
    std::uint16_t completionValueIndex{kUnavailableCompletionValueIndex};
    std::uint8_t socketLane{};
    Availability availability{Availability::unsupported};
    /** Required value for a legacy completion expression; zero when no value is needed. */
    std::int32_t completionValue{};
};

/** The only state a released catalyst exposes to callers. */
struct CompletedCatalyst {
    std::uint8_t socketLane{};
    std::uint16_t completedPlugDefinitionIndex{};
    std::uint16_t effectDefinitionIndex{};
    std::uint16_t acquisitionDefinitionIndex{kUnavailableAcquisitionIndex};
    std::uint16_t completionFlagDefinitionIndex{kUnavailableCompletionFlagIndex};
    std::uint16_t completionValueIndex{kUnavailableCompletionValueIndex};
    std::int32_t completionValue{};
};

/** Result of scanning one effect item's postfix completion expressions. */
enum class CompletionConditionState : std::uint8_t {
    absent = 0,
    present = 1,
    ambiguous = 2,
};

/** One item definition's build-derived completion flag and value. */
struct CompletionCondition {
    std::uint16_t itemDefinitionIndex{};
    std::uint16_t flagDefinitionIndex{kUnavailableCompletionFlagIndex};
    std::uint16_t valueIndex{kUnavailableCompletionValueIndex};
    std::int32_t value{};
    CompletionConditionState state{CompletionConditionState::absent};
};

/** Result of reading one socket type's acquired-state rule. */
enum class AcquisitionState : std::uint8_t {
    absent = 0,
    present = 1,
    ambiguous = 2,
};

/** One socket type's build-derived Family-5 acquisition gate. */
struct AcquisitionGate {
    std::uint16_t socketType{};
    std::uint16_t definitionIndex{kUnavailableAcquisitionIndex};
    AcquisitionState state{AcquisitionState::absent};
};

/** One resolver result. Errors never carry a completed state. */
struct Result {
    Error error{Error::noCatalyst};
    Availability availability{Availability::unsupported};
    CompletedCatalyst completed{};
};

/** Whole-catalog counts plus the first unsafe released relation. */
struct Report {
    std::size_t released{};
    std::size_t placeholder{};
    std::size_t unsupported{};
    Error error{Error::none};
    std::uint32_t itemDefinitionHash{};
    std::uint8_t socketLane{};
};

/** Result of the one catalog-aware catalyst item change. */
enum class ApplyResult : std::uint8_t {
    unchanged = 0,
    completed,
    failed,
};

} // namespace sunrise::state::build_data::items::catalysts
