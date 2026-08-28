#include "world_inspector_graph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../inspection/inspection_descriptors.h"
#include "world_inspector_graph_geometry.h"
#include "world_inspector_label_layout.h"

namespace sunrise::client::ui::world_inspector::graph {
namespace {

namespace dpi = core::ui::scaling::dpi;

constexpr float kCardWidth = graph_geometry::kCardWidth;
constexpr float kCardHeight = graph_geometry::kCardHeight;
constexpr float kMinimumZoom = 0.25F;
constexpr float kMaximumZoom = 2.5F;
constexpr ImU32 kBackground = IM_COL32(11, 12, 17, 255);
constexpr ImU32 kGrid = IM_COL32(32, 34, 43, 170);
constexpr ImU32 kCard = IM_COL32(22, 24, 32, 255);
constexpr ImU32 kCardHovered = IM_COL32(36, 40, 54, 255);
constexpr ImU32 kCardSelected = IM_COL32(48, 44, 30, 255);
constexpr ImU32 kEdge = IM_COL32(138, 145, 162, 200);
constexpr ImU32 kEdgeSelected = IM_COL32(255, 205, 95, 255);
constexpr ImU32 kRelationshipIncoming = IM_COL32(116, 179, 255, 220);
constexpr ImU32 kRelationshipOutgoing = IM_COL32(245, 126, 84, 230);
constexpr ImU32 kText = IM_COL32(231, 231, 224, 255);
constexpr ImU32 kMuted = IM_COL32(145, 149, 158, 255);
constexpr ImU32 kSelection = IM_COL32(228, 181, 79, 255);
constexpr ImU32 kOverviewOwnership = IM_COL32(124, 132, 150, 125);
constexpr ImU32 kOverviewReference = IM_COL32(89, 178, 255, 210);
constexpr ImU32 kOverviewLogic = IM_COL32(245, 126, 84, 225);
constexpr ImU32 kOverviewAuthored = IM_COL32(174, 124, 239, 215);
constexpr ImU32 kOverviewRuntime = IM_COL32(72, 201, 176, 220);
constexpr ImU32 kOverviewGraphLink = IM_COL32(230, 184, 74, 220);
constexpr ImU32 kOverviewVariableRead = IM_COL32(94, 210, 128, 235);
constexpr ImU32 kOverviewVariableWrite = IM_COL32(245, 126, 84, 235);
constexpr ImU32 kOverviewLabelBackground = IM_COL32(11, 12, 17, 220);

[[nodiscard]] float scaled(float value) noexcept {
    return dpi::pixels(value);
}

[[nodiscard]] ImU32 kind_color(inspection::NodeKind kind) noexcept {
    if (kind == inspection::NodeKind::logicVariable) {
        return kOverviewVariableRead;
    }
    if (kind == inspection::NodeKind::logicPlacement) {
        return kOverviewAuthored;
    }
    switch (inspection::descriptor(kind).category) {
    case inspection::NodeCategory::geometry:
        return IM_COL32(72, 201, 176, 255);
    case inspection::NodeCategory::entity:
        return IM_COL32(239, 142, 70, 255);
    case inspection::NodeCategory::trigger:
        return IM_COL32(225, 98, 181, 255);
    case inspection::NodeCategory::audio:
        return IM_COL32(174, 124, 239, 255);
    case inspection::NodeCategory::physics:
        return IM_COL32(86, 150, 238, 255);
    case inspection::NodeCategory::spawn:
        return IM_COL32(230, 184, 74, 255);
    case inspection::NodeCategory::logic:
        return IM_COL32(245, 126, 84, 255);
    default:
        return IM_COL32(137, 146, 164, 255);
    }
}

[[nodiscard]] bool inside(ImVec2 point, ImVec2 minimum, ImVec2 maximum) noexcept {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y
           && point.y <= maximum.y;
}

void rebuild_index(State& state) {
    state.layoutIndex.clear();
    state.layoutIndex.reserve(state.layout.size());
    for (std::size_t index = 0; index < state.layout.size(); ++index) {
        state.layoutIndex.emplace(state.layout[index].id.value, index);
    }
}

[[nodiscard]] const LayoutNode* layout_for(const State& state, inspection::NodeId id) noexcept {
    const auto iterator = state.layoutIndex.find(id.value);
    return iterator == state.layoutIndex.end() || iterator->second >= state.layout.size()
               ? nullptr
               : &state.layout[iterator->second];
}

void fit(State& state, ImVec2 canvasSize) noexcept {
    if (state.layout.empty()) {
        state.pan = {};
        state.zoom = 1.0F;
        state.fitRequested = false;
        return;
    }
    float minimumX = (std::numeric_limits<float>::max)();
    float minimumY = (std::numeric_limits<float>::max)();
    float maximumX = (std::numeric_limits<float>::lowest)();
    float maximumY = (std::numeric_limits<float>::lowest)();
    for (const LayoutNode& node : state.layout) {
        minimumX = (std::min)(minimumX, node.position[0]);
        minimumY = (std::min)(minimumY, node.position[1]);
        maximumX = (std::max)(maximumX, node.position[0]);
        maximumY = (std::max)(maximumY, node.position[1]);
    }
    const float cardWidth = scaled(kCardWidth);
    const float cardHeight = scaled(kCardHeight);
    const float width = (std::max)(1.0F, maximumX - minimumX + cardWidth);
    const float height = (std::max)(1.0F, maximumY - minimumY + cardHeight);
    const float availableWidth = (std::max)(1.0F, canvasSize.x - scaled(24.0F));
    const float availableHeight = (std::max)(1.0F, canvasSize.y - scaled(24.0F));
    state.zoom = std::clamp(
        (std::min)(availableWidth / width, availableHeight / height), kMinimumZoom, 1.4F);
    state.pan = {
        scaled(12.0F) + (availableWidth - width * state.zoom) * 0.5F - minimumX * state.zoom,
        scaled(12.0F) + (availableHeight - height * state.zoom) * 0.5F - minimumY * state.zoom};
    state.fitRequested = false;
}

[[nodiscard]] ImVec2
screen_center(ImVec2 canvasMinimum, const State& state, const LayoutNode& node) noexcept {
    return {canvasMinimum.x + state.pan.x + node.position[0] * state.zoom,
            canvasMinimum.y + state.pan.y + node.position[1] * state.zoom};
}

[[nodiscard]] std::array<ImVec2, 2>
card_rect(ImVec2 canvasMinimum, const State& state, const LayoutNode& node) noexcept {
    const ImVec2 center = screen_center(canvasMinimum, state, node);
    const float width = scaled(kCardWidth) * state.zoom;
    const float height = scaled(kCardHeight) * state.zoom;
    return {{{center.x - width * 0.5F, center.y - height * 0.5F},
             {center.x + width * 0.5F, center.y + height * 0.5F}}};
}

void handle_navigation(State& state,
                       ImVec2 canvasMinimum,
                       ImVec2 canvasSize,
                       bool hoveredCanvas) noexcept {
    if (state.fitRequested) {
        fit(state, canvasSize);
    }
    if (state.centerRequested) {
        if (const LayoutNode* target = layout_for(state, state.centerRequested);
            target != nullptr) {
            state.pan = {canvasSize.x * 0.5F - target->position[0] * state.zoom,
                         canvasSize.y * 0.5F - target->position[1] * state.zoom};
        }
        state.centerRequested = {};
    }
    if (hoveredCanvas && ImGui::GetIO().MouseWheel != 0.0F) {
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const float oldZoom = state.zoom;
        state.zoom = std::clamp(
            state.zoom * std::pow(1.15F, ImGui::GetIO().MouseWheel), kMinimumZoom, kMaximumZoom);
        const float ratio = oldZoom > 0.0F ? state.zoom / oldZoom : 1.0F;
        state.pan = {
            pointer.x - canvasMinimum.x - (pointer.x - canvasMinimum.x - state.pan.x) * ratio,
            pointer.y - canvasMinimum.y - (pointer.y - canvasMinimum.y - state.pan.y) * ratio};
    }
    if (hoveredCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        state.pan.x += ImGui::GetIO().MouseDelta.x;
        state.pan.y += ImGui::GetIO().MouseDelta.y;
    }
}

void draw_background(ImDrawList& drawList,
                     ImVec2 minimum,
                     ImVec2 maximum,
                     const State& state) noexcept {
    drawList.AddRectFilled(minimum, maximum, kBackground);
    const float grid = scaled(48.0F) * state.zoom;
    if (grid <= 4.0F) {
        return;
    }
    const float offsetX = std::fmod(state.pan.x, grid);
    const float offsetY = std::fmod(state.pan.y, grid);
    const float horizontalDistance = maximum.x - minimum.x - offsetX;
    const std::size_t verticalLineCount =
        horizontalDistance > 0.0F ? static_cast<std::size_t>(std::ceil(horizontalDistance / grid))
                                  : 0;
    for (std::size_t line = 0; line < verticalLineCount; ++line) {
        const float x = minimum.x + offsetX + static_cast<float>(line) * grid;
        drawList.AddLine({x, minimum.y}, {x, maximum.y}, kGrid, 1.0F);
    }
    const float verticalDistance = maximum.y - minimum.y - offsetY;
    const std::size_t horizontalLineCount =
        verticalDistance > 0.0F ? static_cast<std::size_t>(std::ceil(verticalDistance / grid)) : 0;
    for (std::size_t line = 0; line < horizontalLineCount; ++line) {
        const float y = minimum.y + offsetY + static_cast<float>(line) * grid;
        drawList.AddLine({minimum.x, y}, {maximum.x, y}, kGrid, 1.0F);
    }
}

void draw_routed_edge(
    ImDrawList& drawList, ImVec2 from, ImVec2 to, ImU32 color, float thickness) noexcept {
    const float control = (std::max)(scaled(28.0F), std::abs(to.x - from.x) * 0.45F);
    drawList.AddBezierCubic(
        from, {from.x + control, from.y}, {to.x - control, to.y}, to, color, thickness);
}

void draw_arrowhead(
    ImDrawList& drawList, ImVec2 to, bool pointsRight, ImU32 color, float thickness) noexcept {
    const float length = scaled(8.0F);
    const float half = scaled(4.0F);
    const float direction = pointsRight ? -1.0F : 1.0F;
    drawList.AddLine(to, {to.x + direction * length, to.y - half}, color, thickness);
    drawList.AddLine(to, {to.x + direction * length, to.y + half}, color, thickness);
}

[[nodiscard]] std::string ellipsize(std::string_view text, float availableWidth) {
    if (ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= availableWidth) {
        return std::string(text);
    }
    std::string value(text);
    while (value.size() > 3U) {
        value.resize(value.size() - 1U);
        std::string candidate = value + "...";
        if (ImGui::CalcTextSize(candidate.c_str()).x <= availableWidth) {
            return candidate;
        }
    }
    return "...";
}

void draw_card(ImDrawList& drawList,
               const inspection::Node& node,
               const std::array<ImVec2, 2>& rect,
               bool selected,
               bool hovered,
               float zoom) {
    const ImU32 background = selected ? kCardSelected : (hovered ? kCardHovered : kCard);
    drawList.AddRectFilled(rect[0], rect[1], background, scaled(4.0F));
    drawList.AddRect(rect[0],
                     rect[1],
                     selected ? kSelection : IM_COL32(52, 56, 70, 255),
                     scaled(1.25F),
                     0,
                     scaled(4.0F));
    drawList.AddRectFilled({rect[0].x, rect[0].y},
                           {rect[0].x + scaled(4.0F), rect[1].y},
                           kind_color(node.kind),
                           scaled(2.0F));
    if (zoom < 0.38F) {
        return;
    }
    const float textWidth = (std::max)(1.0F, rect[1].x - rect[0].x - scaled(18.0F));
    const std::string name = ellipsize(node.name, textWidth);
    drawList.AddText({rect[0].x + scaled(10.0F), rect[0].y + scaled(8.0F)}, kText, name.c_str());
    if (zoom < 0.62F) {
        return;
    }
    std::array<char, 160> status{};
    std::snprintf(status.data(),
                  status.size(),
                  "%s / %s / %zu children",
                  inspection::status_name(node.status),
                  inspection::provenance_name(node.provenance),
                  node.children.size());
    const std::string detail = ellipsize(status.data(), textWidth);
    drawList.AddText(
        {rect[0].x + scaled(10.0F), rect[0].y + scaled(31.0F)}, kMuted, detail.c_str());
}

[[nodiscard]] const inspection::Node* logic_entity(const inspection::Graph& graph,
                                                   inspection::NodeId selected) noexcept {
    const inspection::Node* node = graph.node(selected);
    if (node != nullptr && node->kind == inspection::NodeKind::logicPlacement && node->parent) {
        node = graph.node(node->parent);
    }
    return node != nullptr && node->kind == inspection::NodeKind::logicEntity
                   && node->activityLogicMetadata.has_value()
               ? node
               : nullptr;
}

[[nodiscard]] inspection::NodeId logic_node_for_definition(const inspection::Graph& graph,
                                                           std::uint32_t definitionTag) noexcept {
    for (const inspection::Node& node : graph.nodes()) {
        if (node.kind == inspection::NodeKind::logicEntity && node.activityLogicMetadata.has_value()
            && node.activityLogicMetadata->definitionTag == definitionTag) {
            return node.id;
        }
    }
    return {};
}

/** Hashes the model-owned stable key so cached graph navigation survives NodeId churn. */
[[nodiscard]] std::uint64_t root_identity(const inspection::Graph& graph,
                                          inspection::NodeId id) noexcept {
    if (!id) {
        return 0;
    }
    const inspection::Node* node = graph.node(id);
    if (node == nullptr) {
        return 0;
    }
    const std::uint64_t value = static_cast<std::uint64_t>(inspection::NodeKeyHash{}(node->key));
    return value == 0 ? 1 : value;
}

struct RelationshipLink final {
    inspection::NodeId target{};
    bool outgoing{};
    std::uint32_t occurrenceCount{1};
    std::uint32_t nameHash{};
    std::int32_t selector{-1};
    inspection::RelationKind kind{inspection::RelationKind::reference};
};

[[nodiscard]] std::vector<RelationshipLink>
relationship_links(const inspection::Graph& graph, const inspection::Node& node) {
    std::vector<RelationshipLink> links;
    if (node.activityLogicMetadata.has_value()) {
        for (const inspection::ActivityLogicRelationship& relationship :
             node.activityLogicMetadata->relationships) {
            const inspection::NodeId target =
                logic_node_for_definition(graph, relationship.definitionTag);
            if (target) {
                links.push_back({target,
                                 relationship.outgoing,
                                 relationship.occurrenceCount,
                                 relationship.nameHash,
                                 -1,
                                 inspection::RelationKind::logic});
            }
        }
    }
    for (const inspection::Relation& relation : node.relations) {
        const inspection::NodeId target = graph.resolve(relation.target);
        if (!target) {
            continue;
        }
        // Entity metadata already exposes serialized-name relationships; avoid drawing those
        // twice while retaining authored owner bindings and StateVar references.
        if (relation.kind == inspection::RelationKind::logic) {
            continue;
        }
        links.push_back({target,
                         relation.outgoing,
                         relation.occurrenceCount,
                         relation.nameHash,
                         relation.selector,
                         relation.kind});
    }
    return links;
}

[[nodiscard]] bool relationship_selection(const inspection::Node& node) noexcept {
    return node.kind == inspection::NodeKind::logicVariable
           || (node.kind == inspection::NodeKind::logicGroup && !node.relations.empty())
           || (node.kind == inspection::NodeKind::logicEntity
               && (!node.relations.empty()
                   || (node.activityLogicMetadata.has_value()
                       && !node.activityLogicMetadata->relationships.empty())));
}

[[nodiscard]] ImU32 overview_edge_color(overview::EdgeKind kind) noexcept {
    switch (kind) {
    case overview::EdgeKind::ownership:
        return kOverviewOwnership;
    case overview::EdgeKind::reference:
        return kOverviewReference;
    case overview::EdgeKind::logic:
        return kOverviewLogic;
    case overview::EdgeKind::authoredLink:
        return kOverviewAuthored;
    case overview::EdgeKind::runtimeAssociation:
        return kOverviewRuntime;
    case overview::EdgeKind::activityGraphLink:
        return kOverviewGraphLink;
    case overview::EdgeKind::logicVariableRead:
        return kOverviewVariableRead;
    case overview::EdgeKind::logicVariableWrite:
        return kOverviewVariableWrite;
    }
    return kOverviewOwnership;
}

[[nodiscard]] bool overview_semantic_edge(overview::EdgeKind kind) noexcept {
    return kind == overview::EdgeKind::authoredLink
           || kind == overview::EdgeKind::activityGraphLink
           || kind == overview::EdgeKind::logicVariableRead
           || kind == overview::EdgeKind::logicVariableWrite;
}

[[nodiscard]] ImU32 multiply_alpha(ImU32 color, float factor) noexcept {
    const auto alpha = static_cast<unsigned>((color >> 24U) & 0xFFU);
    const auto adjusted = static_cast<ImU32>(
        std::clamp(static_cast<unsigned>(static_cast<float>(alpha) * factor), 0U, 255U));
    return (color & 0x00FF'FFFFU) | (adjusted << 24U);
}

[[nodiscard]] ImVec2 overview_point(ImVec2 minimum,
                                    const OverviewState& state,
                                    std::size_t index) noexcept {
    if (index >= state.positions.size()) {
        return minimum;
    }
    return {minimum.x + state.pan.x + state.positions[index][0] * state.zoom,
            minimum.y + state.pan.y + state.positions[index][1] * state.zoom};
}

void fit_overview(OverviewState& state, ImVec2 size) noexcept {
    if (state.positions.empty()) {
        state.pan = {size.x * 0.5F, size.y * 0.5F};
        state.zoom = 1.0F;
        state.fitRequested = false;
        return;
    }
    float minimumX = (std::numeric_limits<float>::max)();
    float minimumY = (std::numeric_limits<float>::max)();
    float maximumX = (std::numeric_limits<float>::lowest)();
    float maximumY = (std::numeric_limits<float>::lowest)();
    for (const auto& position : state.positions) {
        minimumX = (std::min)(minimumX, position[0]);
        minimumY = (std::min)(minimumY, position[1]);
        maximumX = (std::max)(maximumX, position[0]);
        maximumY = (std::max)(maximumY, position[1]);
    }
    const float margin = scaled(72.0F);
    const float width = (std::max)(1.0F, maximumX - minimumX);
    const float height = (std::max)(1.0F, maximumY - minimumY);
    state.zoom = std::clamp((std::min)((std::max)(1.0F, size.x - margin * 2.0F) / width,
                                        (std::max)(1.0F, size.y - margin * 2.0F) / height),
                            kMinimumZoom,
                            1.4F);
    state.pan = {size.x * 0.5F - (minimumX + maximumX) * 0.5F * state.zoom,
                 size.y * 0.5F - (minimumY + maximumY) * 0.5F * state.zoom};
    state.fitRequested = false;
}

[[nodiscard]] std::uint64_t overview_input_revision(const inspection::Graph& graph,
                                                    std::uint64_t admissionRevision,
                                                    const overview::ActivityScope& scope,
                                                    std::size_t maximumNodes) noexcept {
    std::uint64_t value = static_cast<std::uint64_t>(graph.generation()) << 32U;
    value ^= admissionRevision;
    value ^= scope.activitySession;
    value ^= static_cast<std::uint64_t>(scope.scenarioTag) << 1U;
    value ^= static_cast<std::uint64_t>(maximumNodes) << 17U;
    value ^= scope.preview ? 0xA5A5A5A5A5A5A5A5ULL : 0ULL;
    value ^= scope.available ? 0x5A5A5A5A5A5A5A5AULL : 0ULL;
    for (const unsigned char character : scope.label) {
        value = (value ^ character) * 1099511628211ULL;
    }
    return value == 0 ? 1 : value;
}

} // namespace

void reset(State& state) noexcept {
    state = {};
    state.zoom = 1.0F;
    state.fitRequested = true;
}

void reset(OverviewState& state) noexcept {
    state = {};
    state.zoom = 1.0F;
    state.fitRequested = true;
}

Result draw_overview(const inspection::Graph& graph,
                     const std::unordered_set<std::uint64_t>& eligible,
                     std::uint64_t admissionRevision,
                     const overview::ActivityScope& scope,
                     std::size_t maximumNodes,
                     inspection::NodeId selected,
                     OverviewState& state) noexcept {
    Result result{};
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 1.0F);
    size.y = (std::max)(size.y, 1.0F);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
    ImGui::InvisibleButton("##current_activity_overview",
                           size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                               | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hoveredCanvas = ImGui::IsItemHovered();

    const std::uint64_t inputRevision =
        overview_input_revision(graph, admissionRevision, scope, maximumNodes);
    const bool inputChanged = state.cachedInputRevision != inputRevision;
    const bool selectionChanged = state.cachedSelected != selected;
    if (inputChanged || selectionChanged) {
        overview::Model next = overview::build(graph, eligible, scope, maximumNodes, selected);
        const bool topologyChanged = next.revision != state.model.revision;
        state.model = std::move(next);
        if (topologyChanged) {
            overview::layout(state.model, state.positions);
        }
        state.cachedInputRevision = inputRevision;
        state.cachedSelected = selected;
        state.fitRequested = state.fitRequested || inputChanged;
    }
    if (state.fitRequested) {
        fit_overview(state, size);
    }
    if (state.centerRequested) {
        const auto found = std::ranges::find_if(state.model.nodes, [&](const overview::Node& node) {
            return node.source == state.centerRequested;
        });
        if (found != state.model.nodes.end()) {
            const std::size_t index = static_cast<std::size_t>(found - state.model.nodes.begin());
            state.pan = {size.x * 0.5F - state.positions[index][0] * state.zoom,
                         size.y * 0.5F - state.positions[index][1] * state.zoom};
        }
        state.centerRequested = {};
    }
    if (hoveredCanvas && ImGui::GetIO().MouseWheel != 0.0F) {
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const float previous = state.zoom;
        state.zoom = std::clamp(
            state.zoom * std::pow(1.15F, ImGui::GetIO().MouseWheel), kMinimumZoom, kMaximumZoom);
        const float ratio = previous > 0.0F ? state.zoom / previous : 1.0F;
        state.pan = {
            pointer.x - minimum.x - (pointer.x - minimum.x - state.pan.x) * ratio,
            pointer.y - minimum.y - (pointer.y - minimum.y - state.pan.y) * ratio};
    }
    if (hoveredCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        state.pan.x += ImGui::GetIO().MouseDelta.x;
        state.pan.y += ImGui::GetIO().MouseDelta.y;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    draw_background(*drawList, minimum, maximum, State{state.pan, state.zoom});
    drawList->PushClipRect(minimum, maximum, true);

    const ImVec2 pointer = ImGui::GetIO().MousePos;
    std::size_t hoveredIndex = (std::numeric_limits<std::size_t>::max)();
    for (std::size_t index = 0; index < state.model.nodes.size(); ++index) {
        const overview::Node& visual = state.model.nodes[index];
        if (visual.hub) {
            continue;
        }
        const ImVec2 point = overview_point(minimum, state, index);
        const float dx = pointer.x - point.x;
        const float dy = pointer.y - point.y;
        if (hoveredCanvas && point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y
            && point.y <= maximum.y
            && dx * dx + dy * dy <= scaled(10.0F) * scaled(10.0F)) {
            hoveredIndex = index;
            result.hovered = visual.source;
        }
    }

    for (const overview::Edge& edge : state.model.edges) {
        if (edge.source >= state.model.nodes.size() || edge.target >= state.model.nodes.size()) {
            continue;
        }
        const overview::Node& sourceVisual = state.model.nodes[edge.source];
        const overview::Node& targetVisual = state.model.nodes[edge.target];
        const bool focused = (selected && (sourceVisual.source == selected
                                            || targetVisual.source == selected))
                             || edge.source == hoveredIndex || edge.target == hoveredIndex;
        if (edge.kind == overview::EdgeKind::ownership && !focused) {
            const inspection::Node* targetNode =
                targetVisual.hub ? nullptr : graph.node(targetVisual.source);
            if (sourceVisual.lane != overview::Lane::context
                || targetVisual.lane != overview::Lane::context || targetNode == nullptr
                || targetNode->children.empty()) {
                continue;
            }
        }
        if ((edge.kind == overview::EdgeKind::reference
             || edge.kind == overview::EdgeKind::logic
             || edge.kind == overview::EdgeKind::runtimeAssociation)
            && !focused && state.zoom < 1.25F) {
            continue;
        }
        const ImVec2 from = overview_point(minimum, state, edge.source);
        const ImVec2 to = overview_point(minimum, state, edge.target);
        const ImU32 color = focused ? multiply_alpha(overview_edge_color(edge.kind), 1.2F)
                                    : overview_edge_color(edge.kind);
        const float thickness = scaled(focused ? 2.0F
                                               : (edge.kind == overview::EdgeKind::ownership ? 1.0F
                                                                                            : 1.6F));
        drawList->AddLine(from, to, color, thickness);
        if (edge.kind != overview::EdgeKind::ownership) {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length > scaled(18.0F)) {
                const float ux = dx / length;
                const float uy = dy / length;
                const ImVec2 tip{to.x - ux * scaled(8.0F), to.y - uy * scaled(8.0F)};
                const ImVec2 side{-uy * scaled(4.0F), ux * scaled(4.0F)};
                const ImVec2 back{tip.x - ux * scaled(7.0F), tip.y - uy * scaled(7.0F)};
                drawList->AddTriangleFilled(tip,
                                            {back.x + side.x, back.y + side.y},
                                            {back.x - side.x, back.y - side.y},
                                            color);
            }
        }
        if (edge.occurrenceCount > 1U && state.zoom >= 0.55F
            && (focused || overview_semantic_edge(edge.kind) || state.zoom >= 1.25F)) {
            std::array<char, 24> count{};
            std::snprintf(count.data(), count.size(), "x%u", edge.occurrenceCount);
            drawList->AddText({(from.x + to.x) * 0.5F + scaled(3.0F),
                               (from.y + to.y) * 0.5F - scaled(9.0F)},
                              color,
                              count.data());
        }
    }

    for (std::size_t index = 0; index < state.model.nodes.size(); ++index) {
        const overview::Node& visual = state.model.nodes[index];
        const ImVec2 point = overview_point(minimum, state, index);
        const inspection::Node* node = visual.hub ? nullptr : graph.node(visual.source);
        const bool isSelected = !visual.hub && visual.source == selected;
        const bool isHovered = index == hoveredIndex;
        const bool branch = node != nullptr && !node->children.empty();
        const bool semantic = visual.lane != overview::Lane::context;
        ImU32 color = visual.hub ? kSelection
                                 : (node != nullptr ? kind_color(node->kind) : kMuted);
        if (!visual.hub && !semantic && !isSelected && !isHovered) {
            color = multiply_alpha(color, branch ? 0.72F : 0.38F);
        }
        drawList->AddCircleFilled(point,
                                  visual.hub ? scaled(8.0F)
                                             : scaled(isSelected || isHovered ? 8.0F
                                                                               : (semantic || branch
                                                                                      ? 6.5F
                                                                                      : 4.0F)),
                                  color,
                                  16);
        if (isSelected) {
            drawList->AddCircle(point, scaled(10.0F), kSelection, 20, scaled(2.0F));
        }
    }

    struct LaneHeading final {
        overview::Lane lane;
        const char* label;
        ImU32 color;
    };
    constexpr std::array<LaneHeading, 4> headings{{
        {overview::Lane::context, "CONTEXT", kMuted},
        {overview::Lane::behaviorRoot, "BEHAVIOR ROOTS", kOverviewLogic},
        {overview::Lane::variable, "VARIABLES", kOverviewVariableRead},
        {overview::Lane::stateVarOwner, "STATEVAR OWNERS", kOverviewAuthored},
    }};
    for (const LaneHeading& heading : headings) {
        float totalX = 0.0F;
        std::size_t count = 0;
        for (std::size_t index = 0; index < state.model.nodes.size(); ++index) {
            if (state.model.nodes[index].lane == heading.lane && !state.model.nodes[index].hub) {
                totalX += state.positions[index][0];
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        const float laneX = minimum.x + state.pan.x
                            + totalX / static_cast<float>(count) * state.zoom;
        const ImVec2 textSize = ImGui::CalcTextSize(heading.label);
        drawList->AddText({laneX - textSize.x * 0.5F, minimum.y + scaled(29.0F)},
                          multiply_alpha(heading.color, 0.85F),
                          heading.label);
    }

    std::vector<label_layout::Candidate> candidates;
    std::vector<std::size_t> labelNodes;
    std::vector<bool> linkedNodes(state.model.nodes.size(), false);
    for (const overview::Edge& edge : state.model.edges) {
        if (edge.kind != overview::EdgeKind::authoredLink
            && edge.kind != overview::EdgeKind::logicVariableRead
            && edge.kind != overview::EdgeKind::logicVariableWrite
            && edge.kind != overview::EdgeKind::activityGraphLink) {
            continue;
        }
        if (edge.source < linkedNodes.size()) {
            linkedNodes[edge.source] = true;
        }
        if (edge.target < linkedNodes.size()) {
            linkedNodes[edge.target] = true;
        }
    }
    candidates.reserve(state.model.nodes.size());
    labelNodes.reserve(state.model.nodes.size());
    for (std::size_t index = 0; index < state.model.nodes.size(); ++index) {
        const overview::Node& visual = state.model.nodes[index];
        const inspection::Node* node = visual.hub ? nullptr : graph.node(visual.source);
        const std::string_view label = visual.hub ? std::string_view(scope.label)
                                                 : (node != nullptr ? std::string_view(node->name)
                                                                    : std::string_view{});
        int priority = -1;
        if (visual.source == selected || index == hoveredIndex) {
            priority = 5;
        } else if (visual.hub) {
            priority = 3;
        } else {
            switch (visual.lane) {
            case overview::Lane::variable:
                priority = 4;
                break;
            case overview::Lane::behaviorRoot:
                priority = linkedNodes[index] ? 3 : (state.zoom >= 1.15F ? 1 : -1);
                break;
            case overview::Lane::stateVarOwner:
                priority = state.zoom >= 0.9F ? 2 : -1;
                break;
            case overview::Lane::context:
                priority = node != nullptr && !node->children.empty() && state.zoom >= 1.15F
                               ? 1
                               : (state.zoom >= 1.65F ? 0 : -1);
                break;
            }
        }
        const ImVec2 point = overview_point(minimum, state, index);
        if (label.empty() || point.x < minimum.x || point.x > maximum.x || point.y < minimum.y
            || point.y > maximum.y || priority < 0) {
            continue;
        }
        const ImVec2 textSize = ImGui::CalcTextSize(label.data(), label.data() + label.size());
        const float desiredX = visual.lane == overview::Lane::behaviorRoot
                                   ? point.x - scaled(9.0F) - textSize.x
                                   : point.x + scaled(9.0F);
        candidates.push_back({desiredX,
                              point.y - textSize.y * 0.5F,
                              textSize.x,
                              textSize.y,
                              0.0F,
                              visual.identity,
                              priority});
        labelNodes.push_back(index);
    }
    std::vector<label_layout::Placement> labels;
    label_layout::place(candidates,
                        {minimum.x + scaled(4.0F),
                         minimum.y + scaled(50.0F),
                         maximum.x - scaled(4.0F),
                         maximum.y - scaled(24.0F)},
                        scaled(3.0F),
                        scaled(32.0F),
                        labels);
    for (const label_layout::Placement& placed : labels) {
        if (placed.candidate >= labelNodes.size()) {
            continue;
        }
        const overview::Node& visual = state.model.nodes[labelNodes[placed.candidate]];
        const inspection::Node* node = visual.hub ? nullptr : graph.node(visual.source);
        const std::string_view label = visual.hub ? std::string_view(scope.label)
                                                 : (node != nullptr ? std::string_view(node->name)
                                                                    : std::string_view{});
        drawList->AddRectFilled({placed.rect.minimumX - scaled(2.0F),
                                 placed.rect.minimumY - scaled(1.0F)},
                                {placed.rect.maximumX + scaled(2.0F),
                                 placed.rect.maximumY + scaled(1.0F)},
                                kOverviewLabelBackground,
                                scaled(2.0F));
        drawList->AddText({placed.rect.minimumX, placed.rect.minimumY},
                          visual.hub ? kSelection : kText,
                          label.data(),
                          label.data() + label.size());
    }
    drawList->PopClipRect();

    if (hoveredCanvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (result.hovered) {
            result.selected = result.hovered;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                state.centerRequested = result.hovered;
            }
        } else {
            result.clearSelection = true;
        }
    }
    if (hoveredCanvas && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && result.hovered) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        if (drag.x * drag.x + drag.y * drag.y <= scaled(4.0F) * scaled(4.0F)) {
            result.context = result.hovered;
        }
    }

    if (!scope.available) {
        drawList->AddText({minimum.x + scaled(12.0F), minimum.y + scaled(12.0F)},
                          kMuted,
                          "Enter a live activity or select an authored activity preview.");
        return result;
    }
    std::array<char, 320> status{};
    std::snprintf(status.data(),
                  status.size(),
                  "%s%s - %zu definitions shown, %zu variables (%zu unresolved), %zu references, "
                  "%zu compacted, %zu omitted",
                  scope.preview ? "AUTHORED PREVIEW - " : "",
                  scope.label.c_str(),
                  state.model.definitionCount,
                  state.model.variableCount,
                  state.model.unresolvedNameCount,
                  state.model.variableAccessEdgeCount,
                  state.model.compacted,
                  state.model.omitted);
    drawList->AddText({minimum.x + scaled(8.0F), minimum.y + scaled(8.0F)}, kMuted, status.data());
    drawList->AddText({minimum.x + scaled(8.0F), maximum.y - scaled(22.0F)},
                      kMuted,
                      "gray ownership  blue reference  orange logic/write  green variable read  "
                      "purple authored  teal runtime  gold graph link");
    if (result.hovered) {
        if (const inspection::Node* node = graph.node(result.hovered); node != nullptr) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(node->name.c_str());
            ImGui::TextDisabled("%s / %s / %s",
                                inspection::kind_name(node->kind),
                                inspection::provenance_name(node->provenance),
                                inspection::status_name(node->status));
            ImGui::EndTooltip();
        }
    }
    return result;
}

Result draw(const inspection::Graph& graph,
            inspection::NodeId root,
            inspection::NodeId selected,
            const std::unordered_set<std::uint64_t>& admitted,
            std::uint64_t admissionRevision,
            std::size_t omittedCount,
            State& state) noexcept {
    Result result{};
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 1.0F);
    size.y = (std::max)(size.y, 1.0F);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
    ImGui::InvisibleButton("##world_inspector_graph",
                           size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                               | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hoveredCanvas = ImGui::IsItemHovered();

    if (state.cachedGeneration != graph.generation()
        || state.cachedLayoutRevision != admissionRevision || state.cachedRoot != root) {
        const bool firstLayout = state.layout.empty();
        // Compare roots by semantic identity, not raw NodeId: snapshot rebuilds renumber
        // nodes when runtime entities spawn in and out, and a raw compare would force a
        // view reset on every churn.
        const std::uint64_t identity = root_identity(graph, root);
        const bool rootChanged = state.cachedRootIdentity != identity;
        graph_layout::compute(graph, root, admitted, state.layout);
        rebuild_index(state);
        state.cachedGeneration = graph.generation();
        state.cachedLayoutRevision = admissionRevision;
        state.cachedRoot = root;
        state.cachedRootIdentity = identity;
        state.fitRequested = state.fitRequested || firstLayout || rootChanged;
    }

    handle_navigation(state, minimum, size, hoveredCanvas);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    draw_background(*drawList, minimum, maximum, state);
    drawList->PushClipRect(minimum, maximum, true);

    for (const LayoutNode& layout : state.layout) {
        const inspection::Node* node = graph.node(layout.id);
        if (node == nullptr || !node->parent || !admitted.contains(node->parent.value)) {
            continue;
        }
        const LayoutNode* parent = layout_for(state, node->parent);
        if (parent == nullptr) {
            continue;
        }
        const auto parentRect = card_rect(minimum, state, *parent);
        const auto childRect = card_rect(minimum, state, layout);
        const ImVec2 from{parentRect[1].x, (parentRect[0].y + parentRect[1].y) * 0.5F};
        const ImVec2 to{childRect[0].x, (childRect[0].y + childRect[1].y) * 0.5F};
        if ((from.x < minimum.x && to.x < minimum.x) || (from.x > maximum.x && to.x > maximum.x)
            || (from.y < minimum.y && to.y < minimum.y)
            || (from.y > maximum.y && to.y > maximum.y)) {
            continue;
        }
        const bool selectedPath = layout.id == selected || node->parent == selected;
        draw_routed_edge(*drawList,
                         from,
                         to,
                         selectedPath ? kEdgeSelected : kEdge,
                         scaled(selectedPath ? 2.1F : 1.35F));
    }

    const ImVec2 pointer = ImGui::GetIO().MousePos;
    for (const LayoutNode& layout : state.layout) {
        const inspection::Node* node = graph.node(layout.id);
        if (node == nullptr) {
            continue;
        }
        const auto rect = card_rect(minimum, state, layout);
        if (rect[1].x < minimum.x || rect[0].x > maximum.x || rect[1].y < minimum.y
            || rect[0].y > maximum.y) {
            continue;
        }
        if (inside(pointer, rect[0], rect[1])) {
            result.hovered = layout.id;
        }
        draw_card(
            *drawList, *node, rect, layout.id == selected, layout.id == result.hovered, state.zoom);
    }
    drawList->PopClipRect();

    if (hoveredCanvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (result.hovered) {
            result.selected = result.hovered;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                state.centerRequested = result.hovered;
            }
        } else {
            result.clearSelection = true;
        }
    }
    if (hoveredCanvas && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && result.hovered) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        if (drag.x * drag.x + drag.y * drag.y <= scaled(4.0F) * scaled(4.0F)) {
            result.context = result.hovered;
        }
    }

    if (!state.layout.empty()) {
        std::array<char, 192> status{};
        std::snprintf(status.data(),
                      status.size(),
                      "Ownership graph - %zu nodes, %zu omitted, %.0f%% zoom",
                      state.layout.size(),
                      omittedCount,
                      static_cast<double>(state.zoom * 100.0F));
        drawList->AddText(
            {minimum.x + scaled(8.0F), minimum.y + scaled(8.0F)}, kMuted, status.data());
        drawList->AddText({minimum.x + scaled(8.0F), maximum.y - scaled(22.0F)},
                          kMuted,
                          "double-click centers, middle-drag pans, wheel zooms");
    } else {
        drawList->AddText({minimum.x + scaled(12.0F), minimum.y + scaled(12.0F)},
                          kMuted,
                          "No admitted nodes in the current graph scope.");
    }
    return result;
}

Result draw_activity_logic_relationships(const inspection::Graph& graph,
                                         inspection::NodeId selected,
                                         State& state) noexcept {
    Result result{};
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 1.0F);
    size.y = (std::max)(size.y, 1.0F);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
    ImGui::InvisibleButton("##activity_logic_relationship_graph",
                           size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                               | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hoveredCanvas = ImGui::IsItemHovered();
    const inspection::Node* center = logic_entity(graph, selected);
    if (center == nullptr) {
        center = graph.node(selected);
    }
    if (center == nullptr || !relationship_selection(*center)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(minimum, maximum, kBackground);
        drawList->AddText({minimum.x + scaled(12.0F), minimum.y + scaled(12.0F)},
                          kMuted,
                          "Select an Activity Logic owner, root, variable, or authored placement.");
        return result;
    }

    const std::vector<RelationshipLink> links = relationship_links(graph, *center);
    std::uint64_t revision = root_identity(graph, center->id);
    for (const RelationshipLink& link : links) {
        revision ^= static_cast<std::uint64_t>(link.target.value) * 0x9E3779B97F4A7C15ULL;
        revision ^= static_cast<std::uint64_t>(link.nameHash) << 17U;
        revision ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(link.selector));
        revision ^= static_cast<std::uint64_t>(link.occurrenceCount) << 33U;
        revision ^= static_cast<std::uint64_t>(link.kind) << 49U;
        revision ^= link.outgoing ? 0xD1B54A32D192ED03ULL : 0;
    }
    if (state.cachedGeneration != graph.generation() || state.cachedLayoutRevision != revision
        || state.cachedRoot != center->id) {
        const bool firstLayout = state.layout.empty();
        const std::uint64_t identity = root_identity(graph, center->id);
        const bool rootChanged = state.cachedRootIdentity != identity;
        state.layout.clear();
        state.layout.push_back(LayoutNode{center->id, {0.0F, 0.0F}, 0});

        struct Target final {
            inspection::NodeId id{};
            std::uint64_t identity{};
            bool incoming{};
            bool outgoing{};
        };
        std::vector<Target> targets;
        std::unordered_map<std::uint64_t, std::size_t> targetIndex;
        for (const RelationshipLink& link : links) {
            if (!link.target) {
                continue;
            }
            const auto [iterator, inserted] = targetIndex.emplace(link.target.value, targets.size());
            if (inserted) {
                const inspection::Node* targetNode = graph.node(link.target);
                targets.push_back(Target{link.target,
                                        targetNode == nullptr
                                            ? link.target.value
                                            : static_cast<std::uint64_t>(
                                                  inspection::NodeKeyHash{}(targetNode->key))});
            }
            Target& target = targets[iterator->second];
            target.outgoing = target.outgoing || link.outgoing;
            target.incoming = target.incoming || !link.outgoing;
        }
        std::ranges::stable_sort(targets, [](const Target& left, const Target& right) {
            if (left.outgoing != right.outgoing) {
                return left.outgoing;
            }
            return left.identity < right.identity;
        });

        std::size_t incomingIndex = 0;
        std::size_t outgoingIndex = 0;
        const std::size_t incomingCount = static_cast<std::size_t>(std::ranges::count_if(
            targets, [](const Target& target) { return target.incoming && !target.outgoing; }));
        const std::size_t outgoingCount = targets.size() - incomingCount;
        const float column = scaled(310.0F);
        const float row = scaled(92.0F);
        for (const Target& target : targets) {
            const bool right = target.outgoing;
            const std::size_t index = right ? outgoingIndex++ : incomingIndex++;
            const std::size_t count = right ? outgoingCount : incomingCount;
            const float y =
                (static_cast<float>(index)
                 - (static_cast<float>((std::max)(count, std::size_t{1})) - 1.0F) * 0.5F)
                * row;
            state.layout.push_back(LayoutNode{target.id, {right ? column : -column, y}, 1});
        }
        rebuild_index(state);
        state.cachedGeneration = graph.generation();
        state.cachedLayoutRevision = revision;
        state.cachedRoot = center->id;
        state.cachedRootIdentity = identity;
        state.fitRequested = state.fitRequested || firstLayout || rootChanged;
    }

    handle_navigation(state, minimum, size, hoveredCanvas);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    draw_background(*drawList, minimum, maximum, state);
    drawList->PushClipRect(minimum, maximum, true);

    std::size_t unresolved = 0;
    const LayoutNode* centerLayout = layout_for(state, center->id);
    if (centerLayout != nullptr) {
        const auto centerRect = card_rect(minimum, state, *centerLayout);
        for (const RelationshipLink& link : links) {
            const LayoutNode* targetLayout = layout_for(state, link.target);
            if (targetLayout == nullptr) {
                ++unresolved;
                continue;
            }
            const auto targetRect = card_rect(minimum, state, *targetLayout);
            ImVec2 from{};
            ImVec2 to{};
            if (link.outgoing) {
                from = {centerRect[1].x, (centerRect[0].y + centerRect[1].y) * 0.5F};
                to = {targetRect[0].x, (targetRect[0].y + targetRect[1].y) * 0.5F};
            } else {
                from = {targetRect[1].x, (targetRect[0].y + targetRect[1].y) * 0.5F};
                to = {centerRect[0].x, (centerRect[0].y + centerRect[1].y) * 0.5F};
            }
            const ImU32 color = link.kind == inspection::RelationKind::logicVariableRead
                                    ? kOverviewVariableRead
                                : link.kind == inspection::RelationKind::logicVariableWrite
                                    ? kOverviewVariableWrite
                                : link.kind == inspection::RelationKind::authoredLink
                                    ? kOverviewAuthored
                                : link.outgoing ? kRelationshipOutgoing : kRelationshipIncoming;
            draw_routed_edge(*drawList, from, to, color, scaled(1.8F));
            draw_arrowhead(*drawList, to, to.x >= from.x, color, scaled(1.6F));
            if (state.zoom >= 0.68F) {
                std::array<char, 112> label{};
                if (link.selector >= 0) {
                    std::snprintf(label.data(),
                                  label.size(),
                                  "0x%08X s%d x%u",
                                  link.nameHash,
                                  link.selector,
                                  link.occurrenceCount);
                } else {
                    std::snprintf(label.data(),
                                  label.size(),
                                  "0x%08X x%u",
                                  link.nameHash,
                                  link.occurrenceCount);
                }
                drawList->AddText(
                    {(from.x + to.x) * 0.5F + scaled(4.0F), (from.y + to.y) * 0.5F - scaled(10.0F)},
                    kMuted,
                    label.data());
            }
        }
    }

    const ImVec2 pointer = ImGui::GetIO().MousePos;
    for (const LayoutNode& layout : state.layout) {
        const inspection::Node* node = graph.node(layout.id);
        if (node == nullptr) {
            continue;
        }
        const auto rect = card_rect(minimum, state, layout);
        if (inside(pointer, rect[0], rect[1])) {
            result.hovered = layout.id;
        }
        draw_card(*drawList,
                  *node,
                  rect,
                  layout.id == center->id,
                  layout.id == result.hovered,
                  state.zoom);
    }
    drawList->PopClipRect();

    if (hoveredCanvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (result.hovered) {
            result.selected = result.hovered;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                state.centerRequested = result.hovered;
            }
        } else {
            result.clearSelection = true;
        }
    }
    if (hoveredCanvas && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && result.hovered) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        if (drag.x * drag.x + drag.y * drag.y <= scaled(4.0F) * scaled(4.0F)) {
            result.context = result.hovered;
        }
    }

    std::array<char, 256> status{};
    std::snprintf(status.data(),
                  status.size(),
                  "Activity Logic relationships - %zu links, %zu unresolved; static authored "
                  "evidence, not execution flow",
                  links.size(),
                  unresolved);
    drawList->AddText({minimum.x + scaled(8.0F), minimum.y + scaled(8.0F)}, kMuted, status.data());
    return result;
}

} // namespace sunrise::client::ui::world_inspector::graph
