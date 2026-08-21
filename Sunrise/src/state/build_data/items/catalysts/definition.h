#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::state::build_data::items::catalysts {

/** The target build contains fewer than 128 exotic weapon catalyst-shaped sockets. */
inline constexpr std::size_t kDefinitionCapacity = 128;

/** Release status from the build-scoped Season 11 availability data. */
enum class Availability : std::uint8_t {
    released = 0,
    placeholder = 1,
    unsupported = 2,
};

/**
 * @param availability Stored release state to check.
 * @return True when the value is one of the declared states.
 */
[[nodiscard]] constexpr bool valid_availability(Availability availability) noexcept {
    return availability == Availability::released || availability == Availability::placeholder
           || availability == Availability::unsupported;
}

/** Safe catalog and resolver outcomes. */
enum class Error : std::uint8_t {
    none = 0,
    noCatalyst,
    placeholderOnly,
    unsupportedBuild,
    missingReleased,
    ambiguousLifecycle,
    invalidSocket,
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
    std::uint8_t socketLane{};
    Availability availability{Availability::unsupported};
};

/** The only state a released catalyst exposes to callers. */
struct CompletedCatalyst {
    std::uint8_t socketLane{};
    std::uint16_t completedPlugDefinitionIndex{};
    std::uint16_t effectDefinitionIndex{};
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
