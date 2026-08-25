#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../state/build_data/worlds/world_catalog.h"

namespace sunrise::server::ui::activity_override::enrichment {

inline constexpr std::size_t kTextCapacity = 96;

struct Summary final {
    std::array<char, kTextCapacity> activityName{};
    std::array<char, kTextCapacity> destination{};
    std::uint8_t activityNameLength{};
    std::uint8_t destinationLength{};
    bool activityPresent{};
    bool activityBuildMatch{};
};

/** Copies optional display metadata; none of these fields authorize a launch selection. */
void resolve(const state::build_data::worlds::Summary& world, Summary& output) noexcept;

} // namespace sunrise::server::ui::activity_override::enrichment
