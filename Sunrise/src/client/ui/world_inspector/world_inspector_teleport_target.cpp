#include "world_inspector_teleport_target.h"

#include <algorithm>
#include <cmath>

namespace sunrise::client::ui::world_inspector::teleport_target {
namespace {

[[nodiscard]] bool finite(const std::array<float, 3>& value) noexcept {
    return std::ranges::all_of(value, [](float lane) { return std::isfinite(lane); });
}

} // namespace

Result resolve(const inspection::Graph& graph, inspection::NodeId selection) noexcept {
    Result result{};
    const inspection::Node* const node = graph.node(selection);
    if (node == nullptr) {
        return result;
    }
    if (!inspection::has_spatial_data(*node)) {
        result.failure = Failure::noSpatialData;
        return result;
    }

    if (node->bounds.has_value()) {
        if (!inspection::bounds_valid(*node->bounds)) {
            result.failure = Failure::invalidBounds;
            return result;
        }
        result.position = inspection::bounds_center(*node->bounds);
        const float observedZ =
            node->transform.has_value() ? node->transform->position[2] : node->bounds->maximum[2];
        result.position[2] = (std::max)(node->bounds->maximum[2], observedZ) + kLandingClearance;
        result.usedBounds = true;
    } else {
        result.position = node->transform->position;
    }

    if (!finite(result.position)) {
        result.failure = Failure::nonFinite;
        return result;
    }
    result.failure = Failure::none;
    return result;
}

const char* failure_name(Failure failure) noexcept {
    switch (failure) {
    case Failure::none:
        return "none";
    case Failure::staleSelection:
        return "The selection is stale for the current Inspector graph.";
    case Failure::noSpatialData:
        return "This selection has no world-space position or bounds.";
    case Failure::invalidBounds:
        return "This selection has invalid world-space bounds.";
    case Failure::nonFinite:
        return "This selection contains a non-finite world-space coordinate.";
    }
    return "The selection cannot be resolved.";
}

} // namespace sunrise::client::ui::world_inspector::teleport_target
