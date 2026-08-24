#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <unordered_set>

#include "../../inspection/inspection_scene.h"

namespace sunrise::client::ui::world_inspector::debug_scene {

using Line = inspection::SceneLine;

/** Hard cap on helper-emitting nodes per pass; the collector stops at this budget. */
inline constexpr std::size_t kMaximumBoxes = 1024;

/** Session inputs for one collection pass; mirrors the live workspace filters. */
struct CollectQuery final {
    inspection::NodeId selected{};
    inspection::NodeId hovered{};
    std::array<float, 3> cameraPosition{};
    std::array<float, 3> detailAnchor{};
    inspection::OverlayPolicy policy;
    inspection::RenderViewSnapshot renderView{};
    inspection::SceneFramePtr previousPresentation{};
    std::size_t maximumBoxes{kMaximumBoxes};

    CollectQuery() = default;
    CollectQuery(inspection::NodeId selection,
                 std::array<float, 3> camera,
                 bool labels,
                 std::size_t maximum) noexcept
        : selected(selection), cameraPosition(camera), detailAnchor(camera), maximumBoxes(maximum) {
        policy.showLabels = labels;
    }
    CollectQuery(inspection::NodeId selection,
                 std::array<float, 3> camera,
                 std::array<float, 3> anchor,
                 inspection::OverlayPolicy overlay,
                 std::size_t maximum,
                 inspection::RenderViewSnapshot view = {},
                 inspection::NodeId hover = {}) noexcept
        : selected(selection), hovered(hover), cameraPosition(camera), detailAnchor(anchor),
          policy(overlay), renderView(view), maximumBoxes(maximum) {}
};

using CollectStats = inspection::OverlayStats;

/** Conservative homogeneous-frustum test using the exact native matrix. */
[[nodiscard]] bool node_intersects_view(const inspection::Node& node,
                                        const inspection::RenderViewSnapshot& view) noexcept;

/**
 * Fills @p output with world-space helper segments (bounds edges, position gizmos)
 * for admitted, unhidden nodes. The selected node is emitted first so it can never
 * be truncated away, and nodes that emit nothing do not consume the box budget.
 */
[[nodiscard]] CollectStats collect(std::span<Line> output,
                                   const inspection::Graph& graph,
                                   const std::unordered_set<std::uint64_t>& admitted,
                                   const std::unordered_set<std::uint64_t>& hidden,
                                   const CollectQuery& query);

/** Builds the immutable helper presentation and its world-space line/glyph batches. */
[[nodiscard]] CollectStats collect(inspection::SceneFrame& frame,
                                   const inspection::Graph& graph,
                                   const std::unordered_set<std::uint64_t>& admitted,
                                   const std::unordered_set<std::uint64_t>& hidden,
                                   const CollectQuery& query);

} // namespace sunrise::client::ui::world_inspector::debug_scene
