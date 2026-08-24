#pragma once

#include <array>
#include <cstdint>

#include "../../inspection/inspection_scene.h"

namespace sunrise::client::ui::world_inspector::viewport {

using Detail = inspection::OverlayDetail;

/** Radius in meters for the "selected and nearby" detail level. */
inline constexpr float kDetailNearbyRadius = 100.0F;

/** World-space point the detail levels measure distance from. */
struct DetailAnchor final {
    std::array<float, 3> center{};
    /** True when the anchor came from the selected node rather than the camera. */
    bool fromSelection{};
};

/**
 * Resolves the detail anchor: the selected node's center when it has spatial data,
 * otherwise the camera position so "selected and nearby" degrades to "near me".
 */
[[nodiscard]] DetailAnchor detail_anchor(const inspection::Graph& graph,
                                         inspection::NodeId selected,
                                         const std::array<float, 3>& cameraPosition) noexcept;

/**
 * Node center used by the detail distance test: the transform position when present,
 * otherwise the bounds center. @return False when the node has no usable center.
 */
[[nodiscard]] bool detail_center(const inspection::Node& node,
                                 std::array<float, 3>& center) noexcept;

/**
 * Decides whether one node's helpers draw under @p detail. Callers must already
 * have applied admission, category, hidden, and helper-data checks; the selected
 * node always draws. @return False only when the detail level suppressed the node.
 */
[[nodiscard]] bool detail_includes(Detail detail,
                                   const inspection::Node& node,
                                   bool isSelected,
                                   const std::array<float, 3>& anchor,
                                   float nearbyRadius = kDetailNearbyRadius) noexcept;

} // namespace sunrise::client::ui::world_inspector::viewport
