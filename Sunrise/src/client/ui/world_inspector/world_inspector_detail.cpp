#include "world_inspector_detail.h"

#include <algorithm>
#include <cmath>

namespace sunrise::client::ui::world_inspector::viewport {
namespace {

[[nodiscard]] float distance_squared(const std::array<float, 3>& left,
                                     const std::array<float, 3>& right) noexcept {
    const float x = left[0] - right[0];
    const float y = left[1] - right[1];
    const float z = left[2] - right[2];
    return x * x + y * y + z * z;
}

} // namespace

DetailAnchor detail_anchor(const inspection::Graph& graph,
                           inspection::NodeId selected,
                           const std::array<float, 3>& cameraPosition) noexcept {
    DetailAnchor anchor{cameraPosition, false};
    const inspection::Node* node = selected ? graph.node(selected) : nullptr;
    if (node == nullptr) {
        return anchor;
    }
    std::array<float, 3> center{};
    if (detail_center(*node, center)) {
        anchor = DetailAnchor{center, true};
    }
    return anchor;
}

bool detail_center(const inspection::Node& node, std::array<float, 3>& center) noexcept {
    if (node.transform.has_value()) {
        center = node.transform->position;
        return std::ranges::all_of(center, [](float lane) { return std::isfinite(lane); });
    }
    if (node.bounds.has_value() && inspection::bounds_valid(*node.bounds)) {
        center = inspection::bounds_center(*node.bounds);
        return true;
    }
    return false;
}

bool detail_includes(Detail detail,
                     const inspection::Node& node,
                     bool isSelected,
                     const std::array<float, 3>& anchor,
                     float nearbyRadius) noexcept {
    if (detail == Detail::all || detail == Detail::adaptive || isSelected) {
        return true;
    }
    std::array<float, 3> center{};
    if (!detail_center(node, center)) {
        return false;
    }
    if (detail == Detail::selectedOnly) {
        return false;
    }
    const float radius = (std::max)(nearbyRadius, 0.0F);
    return distance_squared(center, anchor) <= radius * radius;
}

} // namespace sunrise::client::ui::world_inspector::viewport
