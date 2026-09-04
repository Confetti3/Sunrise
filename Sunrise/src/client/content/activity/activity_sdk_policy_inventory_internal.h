#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "activity_sdk_policy_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::policy_inventory::internal {

/** One capability's fixed gate sets produce at most this many unique refusal reasons. */
inline constexpr std::size_t kMaximumFailureReasonCount = 16;

/** Supports transparent lookup in the snapshot's owned deferred string index. */
struct StringHash final {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept;
};

/** Compares owned and borrowed deferred strings without temporary allocations. */
struct StringEqual final {
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept;
    [[nodiscard]] bool operator()(const std::string& left, const std::string& right) const noexcept;
    [[nodiscard]] bool operator()(const std::string& left, std::string_view right) const noexcept;
    [[nodiscard]] bool operator()(std::string_view left, const std::string& right) const noexcept;
};

/** Collects one capability's bounded refusal reasons before canonical sorting. */
struct FailureReasons final {
    std::array<std::string_view, kMaximumFailureReasonCount> values{};
    std::size_t count{};

    [[nodiscard]] bool add(std::string_view value) noexcept;
    void canonicalize() noexcept;
};

/** One object's complete scenario-local occurrence cardinality. */
struct RouteEvidence final {
    bool hasScenario{};
    bool ambiguous{};
    std::uint32_t invalidOccurrences{};
};

/** @return True when left precedes right by unsigned UTF-8 byte order. */
[[nodiscard]] bool byte_less(std::string_view left, std::string_view right) noexcept;

/** @return True when count can be added to current without exceeding maximum. */
[[nodiscard]] bool can_add(std::size_t current, std::size_t count, std::size_t maximum) noexcept;

/** Converts one owned row interval while retaining the current cursor for an empty range. */
[[nodiscard]] bool make_range(std::size_t first, std::size_t count, format::Range& output) noexcept;

/** @return True for an empty optional value or one bounded canonical UTF-8 value. */
[[nodiscard]] bool valid_text(std::string_view value) noexcept;

/** Checks parent order, identity, bounds, and every borrowed UTF-8 value. */
[[nodiscard]] bool valid_inputs(const Inputs& inputs);

/** Builds complete object route evidence without retaining occurrence strings. */
[[nodiscard]] bool build_routes(const Inputs& inputs, std::vector<RouteEvidence>& output);

} // namespace sunrise::client::content::activity::sdk_generation::policy_inventory::internal
