#pragma once

#include <array>
#include <cstdint>

#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::teleport_target {

inline constexpr float kLandingClearance = 1.0F;

enum class Failure : std::uint8_t {
    none,
    staleSelection,
    noSpatialData,
    invalidBounds,
    nonFinite,
};

struct Result final {
    std::array<float, 3> position{};
    Failure failure{Failure::staleSelection};
    bool usedBounds{};

    [[nodiscard]] bool available() const noexcept {
        return failure == Failure::none;
    }
};

/** Resolves a copied teleport target after revalidating the NodeId generation. */
[[nodiscard]] Result resolve(const inspection::Graph& graph, inspection::NodeId selection) noexcept;

[[nodiscard]] const char* failure_name(Failure failure) noexcept;

} // namespace sunrise::client::ui::world_inspector::teleport_target
