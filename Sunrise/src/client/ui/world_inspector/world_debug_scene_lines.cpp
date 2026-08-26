#include "world_debug_scene_lines.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../inspection/inspection_descriptors.h"
#include "world_inspector_detail.h"

namespace sunrise::client::ui::world_inspector::debug_scene {
namespace {

namespace inspection = ::sunrise::client::inspection;

constexpr std::size_t kBoxEdgePairs = 12;
constexpr std::size_t kDashParts = 6;

struct ClipPoint final {
    float x{};
    float y{};
    float z{};
    float w{};
};

[[nodiscard]] ClipPoint transform_point(const std::array<float, 16>& matrix,
                                        const std::array<float, 3>& point) noexcept {
    return {matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3],
            matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7],
            matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11],
            matrix[12] * point[0] + matrix[13] * point[1] + matrix[14] * point[2] + matrix[15]};
}

[[nodiscard]] bool finite(const ClipPoint& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)
           && std::isfinite(point.w);
}

[[nodiscard]] bool point_in_view(const ClipPoint& point) noexcept {
    if (!finite(point) || point.w <= 1.0e-5F) {
        return false;
    }
    // A small guard band retains point markers whose world-space strokes cross an edge.
    const float margin = point.w * 0.03F;
    return point.x >= -point.w - margin && point.x <= point.w + margin
           && point.y >= -point.w - margin && point.y <= point.w + margin && point.z >= -margin
           && point.z <= point.w + margin;
}

[[nodiscard]] float distance_squared(const inspection::Node& node,
                                     const std::array<float, 3>& camera) noexcept {
    std::array<float, 3> center{};
    const inspection::SpatialEvidence evidence = inspection::spatial_evidence(node);
    if (evidence.bounds.has_value() && inspection::bounds_valid(*evidence.bounds)) {
        center = inspection::bounds_center(*evidence.bounds);
    } else if (evidence.transform.has_value()) {
        center = evidence.transform->position;
    } else {
        return std::numeric_limits<float>::max();
    }
    const float x = center[0] - camera[0];
    const float y = center[1] - camera[1];
    const float z = center[2] - camera[2];
    return x * x + y * y + z * z;
}

void emit_segment(std::span<Line>& output,
                  const std::array<float, 3>& start,
                  const std::array<float, 3>& end,
                  const std::array<float, 4>& color) noexcept {
    if (output.empty()) {
        return;
    }
    output[0].first.position = start;
    output[0].first.color = color;
    output[0].second.position = end;
    output[0].second.color = color;
    output = output.subspan(1);
}

/** Emits one box edge; catalog-provenance bounds keep their dashed disclosure. */
void emit_edge(std::span<Line>& output,
               const std::array<float, 3>& start,
               const std::array<float, 3>& end,
               const std::array<float, 4>& color,
               bool dashed) noexcept {
    if (!dashed) {
        emit_segment(output, start, end, color);
        return;
    }
    for (std::size_t part = 0; part < kDashParts; part += 2U) {
        const float first = static_cast<float>(part) / static_cast<float>(kDashParts);
        const float second = static_cast<float>(part + 1U) / static_cast<float>(kDashParts);
        emit_segment(output,
                     {start[0] + (end[0] - start[0]) * first,
                      start[1] + (end[1] - start[1]) * first,
                      start[2] + (end[2] - start[2]) * first},
                     {start[0] + (end[0] - start[0]) * second,
                      start[1] + (end[1] - start[1]) * second,
                      start[2] + (end[2] - start[2]) * second},
                     color);
    }
}

void emit_box(std::span<Line>& output,
              const inspection::Bounds& bounds,
              const std::array<float, 4>& color,
              bool dashed) noexcept {
    const std::array<std::array<float, 3>, 8> corners{{
        {{bounds.minimum[0], bounds.minimum[1], bounds.minimum[2]}},
        {{bounds.maximum[0], bounds.minimum[1], bounds.minimum[2]}},
        {{bounds.maximum[0], bounds.maximum[1], bounds.minimum[2]}},
        {{bounds.minimum[0], bounds.maximum[1], bounds.minimum[2]}},
        {{bounds.minimum[0], bounds.minimum[1], bounds.maximum[2]}},
        {{bounds.maximum[0], bounds.minimum[1], bounds.maximum[2]}},
        {{bounds.maximum[0], bounds.maximum[1], bounds.maximum[2]}},
        {{bounds.minimum[0], bounds.maximum[1], bounds.maximum[2]}},
    }};
    static constexpr std::size_t kEdges[kBoxEdgePairs][2]{
        {0, 1},
        {1, 2},
        {2, 3},
        {3, 0},
        {4, 5},
        {5, 6},
        {6, 7},
        {7, 4},
        {0, 4},
        {1, 5},
        {2, 6},
        {3, 7},
    };
    for (const auto& [from, to] : kEdges) {
        emit_edge(output, corners[from], corners[to], color, dashed);
    }
}

[[nodiscard]] inspection::HelperGlyph glyph_for(const inspection::Node& node) noexcept {
    if (node.kind == inspection::NodeKind::logicPlacement
        && node.activityLogicMetadata.has_value()) {
        switch (node.activityLogicMetadata->role) {
        case 0:
            return inspection::HelperGlyph::action;
        case 1:
            return inspection::HelperGlyph::target;
        case 2:
            return inspection::HelperGlyph::competitive;
        case 3:
            return inspection::HelperGlyph::condition;
        case 4:
            return inspection::HelperGlyph::device;
        case 5:
            return inspection::HelperGlyph::interactable;
        case 6:
            return inspection::HelperGlyph::objective;
        case 7:
            return inspection::HelperGlyph::spatial;
        case 8:
            return inspection::HelperGlyph::spawnRule;
        case 9:
            return inspection::HelperGlyph::squad;
        case 10:
            return inspection::HelperGlyph::trigger;
        default:
            return inspection::HelperGlyph::unknown;
        }
    }
    switch (node.kind) {
    case inspection::NodeKind::runtimeEntity:
        return inspection::HelperGlyph::runtimeEntity;
    case inspection::NodeKind::trigger:
        return inspection::HelperGlyph::trigger;
    case inspection::NodeKind::audio:
        return inspection::HelperGlyph::audio;
    case inspection::NodeKind::physics:
        return inspection::HelperGlyph::physics;
    case inspection::NodeKind::geometry:
    default:
        return inspection::HelperGlyph::spawn;
    }
}

[[nodiscard]] bool representative_center(const inspection::Node& node,
                                         bool selected,
                                         const inspection::OverlayPolicy& policy,
                                         std::array<float, 3>& center,
                                         bool& hasBounds) noexcept {
    const inspection::SpatialEvidence evidence = inspection::spatial_evidence(node);
    hasBounds = evidence.bounds.has_value() && inspection::bounds_valid(*evidence.bounds)
                && (policy.showKnownBounds || selected);
    if (hasBounds) {
        center = inspection::bounds_center(*evidence.bounds);
        return true;
    }
    const bool centerAllowed =
        evidence.transform.has_value()
        && (node.kind != inspection::NodeKind::trigger || policy.showTriggerCenters || selected);
    if (centerAllowed) {
        center = evidence.transform->position;
        return true;
    }
    return false;
}

[[nodiscard]] bool project_center(const std::array<float, 3>& center,
                                  const inspection::RenderViewSnapshot& view,
                                  float& screenX,
                                  float& screenY,
                                  float& depth) noexcept {
    if (!view.valid || !view.exactNative || view.viewport.width <= 0.0F
        || view.viewport.height <= 0.0F) {
        return false;
    }
    const ClipPoint clip = transform_point(view.viewProjection, center);
    if (!point_in_view(clip)) {
        return false;
    }
    const float inverseW = 1.0F / clip.w;
    screenX = view.viewport.x + (clip.x * inverseW * 0.5F + 0.5F) * view.viewport.width;
    screenY = view.viewport.y + (0.5F - clip.y * inverseW * 0.5F) * view.viewport.height;
    depth = clip.z * inverseW;
    return std::isfinite(screenX) && std::isfinite(screenY) && std::isfinite(depth);
}

[[nodiscard]] bool runtime_evidence(const inspection::Node& node) noexcept {
    return node.transformRuntime || node.provenance == inspection::Provenance::runtime
           || node.producer == inspection::Producer::localPlayer
           || node.producer == inspection::Producer::objectSystem;
}

/**
 * Emits every in-scene helper for one node.
 * @return True when at least one line was written (only then the node counts
 *         against the box budget).
 */
bool emit_node(inspection::SceneFrame& frame,
               std::span<Line>& output,
               const inspection::Node& node,
               bool isSelected,
               bool isHovered,
               bool focusContext,
               bool focusActive,
               bool markerOnly,
               std::uint8_t priority,
               const CollectQuery& query) {
    Line* const firstLine = output.data();
    const std::size_t before = output.size();
    const inspection::SpatialEvidence evidence = inspection::spatial_evidence(node);
    std::array<float, 3> center{};
    bool hasBounds = false;
    if (!representative_center(node, isSelected, query.policy, center, hasBounds)) {
        return false;
    }

    const inspection::HelperGlyph glyph = glyph_for(node);
    const inspection::HelperMarkerShape shape = inspection::helper_marker_shape(glyph);
    std::array<float, 4> color = inspection::helper_role_color(glyph);
    if (!isSelected && !isHovered) {
        color[3] = query.policy.baseOpacity
                   * (focusActive && !focusContext ? query.policy.focusContextOpacity : 1.0F);
    }

    const bool drawBounds = hasBounds && !markerOnly;
    if (drawBounds) {
        // Real bounds: draw full AABB + small local axes at center
        const bool isDashed = !node.boundsProvenance.has_value()
                              || *node.boundsProvenance != inspection::Provenance::runtime;
        std::array<float, 4> volumeColor = inspection::helper_volume_color();
        volumeColor[3] = color[3];
        emit_box(output, *evidence.bounds, volumeColor, isDashed && !isSelected);

        // Small local axes at bounds center (only for selected or when labels on)
        const bool authoredOrientationAllowed = node.provenance != inspection::Provenance::catalog
                                                || query.policy.showAuthoredOrientation;
        if ((isSelected || query.policy.showLabels) && authoredOrientationAllowed) {
            constexpr float axisReach = 1.0F;
            emit_segment(output,
                         center,
                         {center[0] + axisReach, center[1], center[2]},
                         {1.0F, 0.25F, 0.25F, 1.0F});
            emit_segment(output,
                         center,
                         {center[0], center[1] + axisReach, center[2]},
                         {0.25F, 1.0F, 0.40F, 1.0F});
            emit_segment(output,
                         center,
                         {center[0], center[1], center[2] + axisReach},
                         {0.25F, 0.50F, 1.0F, 1.0F});
        }
    } else {
        frame.glyphs.push_back({center,
                                color,
                                node.id,
                                0,
                                glyph,
                                shape,
                                query.policy.glyphSizePixels,
                                isSelected,
                                isHovered,
                                isSelected});
    }
    const std::size_t emitted = before - output.size();
    for (std::size_t index = 0; index < emitted; ++index) {
        firstLine[index].owner = node.id;
        firstLine[index].selected = isSelected;
        firstLine[index].hovered = isHovered;
        firstLine[index].alwaysVisible = isSelected;
    }
    float screenX = 0.0F;
    float screenY = 0.0F;
    float depth = 0.0F;
    (void)project_center(center, query.renderView, screenX, screenY, depth);
    frame.presentation.push_back({node.id,
                                  glyph,
                                  shape,
                                  center,
                                  color,
                                  screenX,
                                  screenY,
                                  depth,
                                  drawBounds,
                                  isSelected,
                                  isHovered,
                                  focusContext,
                                  priority,
                                  inspection::PresentationCullReason::none});
    return emitted != 0 || !drawBounds;
}

[[nodiscard]] std::size_t line_requirement(const inspection::Node& node,
                                           bool selected,
                                           const inspection::OverlayPolicy& policy) noexcept {
    const inspection::SpatialEvidence evidence = inspection::spatial_evidence(node);
    const bool bounds = evidence.bounds.has_value() && inspection::bounds_valid(*evidence.bounds)
                        && (policy.showKnownBounds || selected);
    const bool center =
        evidence.transform.has_value()
        && (node.kind != inspection::NodeKind::trigger || policy.showTriggerCenters || selected);
    if (!bounds && !center) {
        return 0;
    }
    std::size_t lines = 0;
    if (bounds) {
        const bool dashed = !node.boundsProvenance.has_value()
                            || *node.boundsProvenance != inspection::Provenance::runtime;
        lines = kBoxEdgePairs * (dashed && !selected ? 3U : 1U);
        lines += selected || policy.showLabels ? 3U : 0U;
    }
    return lines;
}

[[nodiscard]] bool has_representation(const inspection::Node& node,
                                      bool selected,
                                      const inspection::OverlayPolicy& policy) noexcept {
    std::array<float, 3> center{};
    bool hasBounds = false;
    return representative_center(node, selected, policy, center, hasBounds);
}

} // namespace

bool node_intersects_view(const inspection::Node& node,
                          const inspection::RenderViewSnapshot& view) noexcept {
    if (!view.valid || !view.exactNative) {
        return true;
    }
    const inspection::SpatialEvidence evidence = inspection::spatial_evidence(node);
    if (evidence.bounds.has_value() && inspection::bounds_valid(*evidence.bounds)) {
        const inspection::Bounds& bounds = *evidence.bounds;
        const std::array<std::array<float, 3>, 8> corners{{
            {{bounds.minimum[0], bounds.minimum[1], bounds.minimum[2]}},
            {{bounds.maximum[0], bounds.minimum[1], bounds.minimum[2]}},
            {{bounds.maximum[0], bounds.maximum[1], bounds.minimum[2]}},
            {{bounds.minimum[0], bounds.maximum[1], bounds.minimum[2]}},
            {{bounds.minimum[0], bounds.minimum[1], bounds.maximum[2]}},
            {{bounds.maximum[0], bounds.minimum[1], bounds.maximum[2]}},
            {{bounds.maximum[0], bounds.maximum[1], bounds.maximum[2]}},
            {{bounds.minimum[0], bounds.maximum[1], bounds.maximum[2]}},
        }};
        std::array<ClipPoint, corners.size()> clip{};
        for (std::size_t index = 0; index < corners.size(); ++index) {
            clip[index] = transform_point(view.viewProjection, corners[index]);
            if (!finite(clip[index])) {
                return true;
            }
        }
        const auto all = [&](const auto& predicate) noexcept {
            return std::all_of(clip.begin(), clip.end(), predicate);
        };
        // D3D homogeneous clip volume: -w <= x/y <= w and 0 <= z <= w.
        // Reject only when every AABB corner lies beyond the same plane, retaining
        // boxes that cross a plane or surround the camera.
        return !all([](const ClipPoint& point) { return point.w <= 1.0e-5F; })
               && !all([](const ClipPoint& point) { return point.x < -point.w; })
               && !all([](const ClipPoint& point) { return point.x > point.w; })
               && !all([](const ClipPoint& point) { return point.y < -point.w; })
               && !all([](const ClipPoint& point) { return point.y > point.w; })
               && !all([](const ClipPoint& point) { return point.z < 0.0F; })
               && !all([](const ClipPoint& point) { return point.z > point.w; });
    }
    return evidence.transform.has_value()
               ? point_in_view(transform_point(view.viewProjection, evidence.transform->position))
               : true;
}

CollectStats collect(inspection::SceneFrame& frame,
                     const inspection::Graph& graph,
                     const std::unordered_set<std::uint64_t>& admitted,
                     const std::unordered_set<std::uint64_t>& hidden,
                     const CollectQuery& query) {
    CollectStats stats{};
    frame.glyphs.clear();
    frame.presentation.clear();
    frame.picks.clear();
    frame.lineWidthPixels = query.policy.lineWidthPixels;
    frame.glyphs.reserve((std::min)(query.maximumBoxes, std::size_t{1024}));
    frame.presentation.reserve((std::min)(query.maximumBoxes, std::size_t{1024}));
    const std::size_t lineCapacity = frame.lines.size();
    std::span<Line> remaining(frame.lines.data(), frame.lines.size());
    // "All admitted" means all spatially representable nodes, up to the immutable
    // safety ceiling. The configurable density cap belongs to the filtered modes.
    const std::size_t requestedLimit =
        query.policy.detail == inspection::OverlayDetail::all
            ? std::size_t{1024}
            : static_cast<std::size_t>((std::clamp)(query.policy.maximumVisibleNodes, 32U, 1024U));
    const std::size_t nodeLimit = (std::min)(query.maximumBoxes, requestedLimit);

    const auto preview_map_matches = [&query](const inspection::Node& node) noexcept {
        return !node.source.authoredPreview
               || (!query.liveMapFamily.empty() && node.source.mapStem == query.liveMapFamily);
    };

    const inspection::NodeId focusId = query.selected ? query.selected : query.hovered;
    const inspection::Node* focusNode = focusId ? graph.node(focusId) : nullptr;
    if (focusNode != nullptr && !preview_map_matches(*focusNode)) {
        focusNode = nullptr;
    }
    std::array<float, 3> focusCenter{};
    bool ignoredBounds = false;
    const bool hasFocusCenter =
        focusNode != nullptr
        && representative_center(*focusNode, true, query.policy, focusCenter, ignoredBounds);
    const auto is_focus_context = [&](const inspection::Node& node) noexcept {
        if (focusNode == nullptr) {
            return false;
        }
        if (node.id == focusId || node.parent == focusId || focusNode->parent == node.id
            || std::ranges::find(focusNode->children, node.id) != focusNode->children.end()
            || std::ranges::find(node.children, focusId) != node.children.end()) {
            return true;
        }
        for (const inspection::Relation& relation : focusNode->relations) {
            if (graph.resolve(relation.target) == node.id) {
                return true;
            }
        }
        if (!hasFocusCenter) {
            return false;
        }
        std::array<float, 3> center{};
        bool hasBounds = false;
        if (!representative_center(node, false, query.policy, center, hasBounds)) {
            return false;
        }
        const float x = center[0] - focusCenter[0];
        const float y = center[1] - focusCenter[1];
        const float z = center[2] - focusCenter[2];
        return x * x + y * y + z * z <= 64.0F;
    };

    const auto priority_for = [&](const inspection::Node& node) noexcept {
        if (query.policy.detail != inspection::OverlayDetail::adaptive || is_focus_context(node)) {
            return 0U;
        }
        if (runtime_evidence(node)) {
            return 1U;
        }
        const inspection::SpatialEvidence evidence = inspection::spatial_evidence(node);
        return evidence.bounds.has_value() && inspection::bounds_valid(*evidence.bounds) ? 2U : 3U;
    };
    const auto record_cull = [&](const inspection::Node& node,
                                 inspection::PresentationCullReason reason) {
        std::array<float, 3> center{};
        bool hasBounds = false;
        (void)representative_center(node, false, query.policy, center, hasBounds);
        float screenX = 0.0F;
        float screenY = 0.0F;
        float depth = 0.0F;
        (void)project_center(center, query.renderView, screenX, screenY, depth);
        const inspection::HelperGlyph glyph = glyph_for(node);
        const inspection::HelperMarkerShape shape = inspection::helper_marker_shape(glyph);
        std::array<float, 4> color = inspection::helper_role_color(glyph);
        const bool context = is_focus_context(node);
        color[3] = query.policy.baseOpacity
                   * (focusNode != nullptr && !context ? query.policy.focusContextOpacity : 1.0F);
        frame.presentation.push_back({node.id,
                                      glyph,
                                      shape,
                                      center,
                                      color,
                                      screenX,
                                      screenY,
                                      depth,
                                      hasBounds,
                                      false,
                                      false,
                                      context,
                                      static_cast<std::uint8_t>(priority_for(node)),
                                      reason});
    };

    const auto eligible = [&](const inspection::Node& node, bool selected) noexcept {
        if (!preview_map_matches(node)) {
            ++stats.categoryFilteredNodes;
            return false;
        }
        if (!selected && !admitted.contains(node.id.value)) {
            return false;
        }
        if (!selected && !inspection::overlay_category_enabled(node.kind, query.policy)) {
            ++stats.categoryFilteredNodes;
            record_cull(node, inspection::PresentationCullReason::category);
            return false;
        }
        if (!selected && hidden.contains(node.id.value)) {
            ++stats.hiddenNodes;
            record_cull(node, inspection::PresentationCullReason::hidden);
            return false;
        }
        const std::array<float, 3>& detailAnchor =
            query.policy.detail == inspection::OverlayDetail::cameraNearby ? query.cameraPosition
                                                                           : query.detailAnchor;
        if (!selected
            && !viewport::detail_includes(
                query.policy.detail, node, false, detailAnchor, query.policy.nearbyRadius)) {
            ++stats.detailFilteredNodes;
            if (query.policy.detail == inspection::OverlayDetail::selectedNearby
                || query.policy.detail == inspection::OverlayDetail::cameraNearby) {
                ++stats.distanceFilteredNodes;
                record_cull(node, inspection::PresentationCullReason::distance);
            } else {
                record_cull(node, inspection::PresentationCullReason::detail);
            }
            return false;
        }
        if (!selected && !node_intersects_view(node, query.renderView)) {
            ++stats.viewFilteredNodes;
            record_cull(node, inspection::PresentationCullReason::offscreen);
            return false;
        }
        if (!has_representation(node, selected, query.policy)) {
            ++stats.representationFilteredNodes;
            record_cull(node, inspection::PresentationCullReason::representation);
            return false;
        }
        return true;
    };

    const auto emit =
        [&](const inspection::Node& node, bool selected, bool hovered, std::uint8_t priority) {
            const std::size_t required = line_requirement(node, selected, query.policy);
            const bool markerOnly = !selected && required > remaining.size();
            if (markerOnly) {
                // Preserve the node as a compact pickable pin when its full AABB no
                // longer fits. Bounds are optional detail; identities are not.
                ++stats.partialNodes;
                stats.truncated = true;
            }
            if (selected && required > remaining.size()) {
                ++stats.partialNodes;
                stats.truncated = true;
            }
            const bool context = selected || hovered || is_focus_context(node);
            if (context && focusNode != nullptr && !selected && !hovered) {
                ++stats.focusContextNodes;
            }
            if (emit_node(frame,
                          remaining,
                          node,
                          selected,
                          hovered,
                          context,
                          focusNode != nullptr,
                          markerOnly,
                          priority,
                          query)) {
                ++stats.nodesWithLines;
                ++stats.shownNodes;
                return true;
            }
            return false;
        };

    // The selected node is emitted first so budget truncation can never drop it.
    if (query.selected) {
        const inspection::Node* selected = graph.node(query.selected);
        if (selected != nullptr && eligible(*selected, true)) {
            (void)emit(*selected, true, false, 0U);
        }
    }

    // Hover is the second priority tier. It previews the exact helper that a click
    // will select, and remains ahead of every ordinary node under truncation.
    if (query.hovered && query.hovered != query.selected && stats.nodesWithLines < nodeLimit) {
        const inspection::Node* hovered = graph.node(query.hovered);
        if (hovered != nullptr && eligible(*hovered, false)) {
            (void)emit(*hovered, false, true, 0U);
        }
    }

    struct Candidate final {
        const inspection::Node* node{};
        std::size_t required{};
        float distanceSquared{};
        float screenX{};
        float screenY{};
        unsigned priority{};
        bool retained{};
        bool projected{};
    };
    std::vector<Candidate> candidates;
    std::unordered_set<std::uint64_t> retainedNodes;
    if (query.previousPresentation) {
        retainedNodes.reserve(query.previousPresentation->presentation.size());
        for (const inspection::SceneFrame::PresentationEntry& entry :
             query.previousPresentation->presentation) {
            if (entry.cullingReason == inspection::PresentationCullReason::none) {
                retainedNodes.insert(entry.node.value);
            }
        }
    }
    const std::vector<inspection::Node>& nodes = graph.nodes();
    candidates.reserve(nodes.size());
    for (const inspection::Node& node : nodes) {
        if (node.id != query.selected && node.id != query.hovered && eligible(node, false)) {
            std::array<float, 3> center{};
            bool hasBounds = false;
            (void)representative_center(node, false, query.policy, center, hasBounds);
            float screenX = 0.0F;
            float screenY = 0.0F;
            float depth = 0.0F;
            const bool projected =
                project_center(center, query.renderView, screenX, screenY, depth);
            const unsigned priority = priority_for(node);
            candidates.push_back({&node,
                                  line_requirement(node, false, query.policy),
                                  distance_squared(node, query.cameraPosition),
                                  screenX,
                                  screenY,
                                  priority,
                                  retainedNodes.contains(node.id.value),
                                  projected});
        }
    }
    std::stable_sort(
        candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
            if (left.priority != right.priority) {
                return left.priority < right.priority;
            }
            if (left.retained != right.retained) {
                return left.retained;
            }
            if (left.distanceSquared != right.distanceSquared) {
                return left.distanceSquared < right.distanceSquared;
            }
            return left.node->id.value < right.node->id.value;
        });
    std::unordered_map<std::uint64_t, unsigned> screenCells;
    screenCells.reserve((std::min)(candidates.size(), nodeLimit));
    const float cellSize = (std::max)(32.0F, query.policy.glyphSizePixels * 3.0F);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (stats.nodesWithLines >= nodeLimit) {
            for (std::size_t omitted = index; omitted < candidates.size(); ++omitted) {
                record_cull(*candidates[omitted].node, inspection::PresentationCullReason::budget);
            }
            stats.omittedNodes += candidates.size() - index;
            stats.truncated = true;
            break;
        }
        const Candidate& candidate = candidates[index];
        if (query.policy.detail == inspection::OverlayDetail::adaptive && candidate.projected
            && candidate.priority != 0U) {
            const auto cellX = static_cast<std::int32_t>(std::floor(candidate.screenX / cellSize));
            const auto cellY = static_cast<std::int32_t>(std::floor(candidate.screenY / cellSize));
            const std::uint64_t cell =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellX)) << 32U)
                | static_cast<std::uint32_t>(cellY);
            unsigned& count = screenCells[cell];
            if (count >= 2U) {
                ++stats.densityFilteredNodes;
                record_cull(*candidate.node, inspection::PresentationCullReason::density);
                continue;
            }
            ++count;
        }
        (void)emit(*candidate.node, false, false, static_cast<std::uint8_t>(candidate.priority));
    }
    stats.lines = lineCapacity - remaining.size();
    frame.lines.resize(stats.lines);
    return stats;
}

CollectStats collect(std::span<Line> output,
                     const inspection::Graph& graph,
                     const std::unordered_set<std::uint64_t>& admitted,
                     const std::unordered_set<std::uint64_t>& hidden,
                     const CollectQuery& query) {
    inspection::SceneFrame frame;
    frame.lines.resize(output.size());
    const CollectStats stats = collect(frame, graph, admitted, hidden, query);
    std::ranges::copy(frame.lines, output.begin());
    return stats;
}

} // namespace sunrise::client::ui::world_inspector::debug_scene
