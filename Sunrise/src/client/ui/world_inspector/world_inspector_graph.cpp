#include "world_inspector_graph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::client::ui::world_inspector::graph {
namespace {

namespace dpi = core::ui::scaling::dpi;

constexpr float kCardWidth = 190.0F;
constexpr float kCardHeight = 58.0F;
constexpr float kMinimumZoom = 0.25F;
constexpr float kMaximumZoom = 2.5F;
constexpr ImU32 kBackground = IM_COL32(11, 12, 17, 255);
constexpr ImU32 kGrid = IM_COL32(32, 34, 43, 170);
constexpr ImU32 kCard = IM_COL32(25, 27, 35, 245);
constexpr ImU32 kCardHovered = IM_COL32(42, 44, 56, 250);
constexpr ImU32 kCardSelected = IM_COL32(54, 48, 35, 250);
constexpr ImU32 kEdge = IM_COL32(119, 125, 141, 180);
constexpr ImU32 kEdgeSelected = IM_COL32(228, 181, 79, 245);
constexpr ImU32 kRelationshipIncoming = IM_COL32(116, 179, 255, 220);
constexpr ImU32 kRelationshipOutgoing = IM_COL32(245, 126, 84, 230);
constexpr ImU32 kText = IM_COL32(231, 231, 224, 255);
constexpr ImU32 kMuted = IM_COL32(145, 149, 158, 255);
constexpr ImU32 kSelection = IM_COL32(228, 181, 79, 255);

[[nodiscard]] float scaled(float value) noexcept {
    return dpi::pixels(value);
}

[[nodiscard]] ImU32 kind_color(inspection::NodeKind kind) noexcept {
    switch (kind) {
    case inspection::NodeKind::geometry: return IM_COL32(72, 201, 176, 255);
    case inspection::NodeKind::terrain: return IM_COL32(112, 190, 92, 255);
    case inspection::NodeKind::runtimeEntity: return IM_COL32(239, 142, 70, 255);
    case inspection::NodeKind::trigger: return IM_COL32(225, 98, 181, 255);
    case inspection::NodeKind::audio: return IM_COL32(174, 124, 239, 255);
    case inspection::NodeKind::physics: return IM_COL32(86, 150, 238, 255);
    case inspection::NodeKind::spawnGroup:
    case inspection::NodeKind::spawnSet:
    case inspection::NodeKind::spawnPoint: return IM_COL32(230, 184, 74, 255);
    case inspection::NodeKind::activityLogic: return IM_COL32(245, 126, 84, 255);
    case inspection::NodeKind::logicGroup: return IM_COL32(203, 118, 245, 255);
    case inspection::NodeKind::logicEntity: return IM_COL32(245, 170, 92, 255);
    case inspection::NodeKind::logicPlacement: return IM_COL32(245, 105, 92, 255);
    case inspection::NodeKind::light: return IM_COL32(255, 226, 117, 255);
    default: return IM_COL32(137, 146, 164, 255);
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

[[nodiscard]] const LayoutNode* layout_for(const State& state,
                                           inspection::NodeId id) noexcept {
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
    state.zoom = std::clamp((std::min)(availableWidth / width, availableHeight / height),
                            kMinimumZoom,
                            1.4F);
    state.pan = {scaled(12.0F) + (availableWidth - width * state.zoom) * 0.5F
                     - minimumX * state.zoom,
                 scaled(12.0F) + (availableHeight - height * state.zoom) * 0.5F
                     - minimumY * state.zoom};
    state.fitRequested = false;
}

[[nodiscard]] ImVec2 screen_center(ImVec2 canvasMinimum,
                                   const State& state,
                                   const LayoutNode& node) noexcept {
    return {canvasMinimum.x + state.pan.x + node.position[0] * state.zoom,
            canvasMinimum.y + state.pan.y + node.position[1] * state.zoom};
}

[[nodiscard]] std::array<ImVec2, 2> card_rect(ImVec2 canvasMinimum,
                                              const State& state,
                                              const LayoutNode& node) noexcept {
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
        if (const LayoutNode* target = layout_for(state, state.centerRequested); target != nullptr) {
            state.pan = {canvasSize.x * 0.5F - target->position[0] * state.zoom,
                         canvasSize.y * 0.5F - target->position[1] * state.zoom};
        }
        state.centerRequested = {};
    }
    if (hoveredCanvas && ImGui::GetIO().MouseWheel != 0.0F) {
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const float oldZoom = state.zoom;
        state.zoom = std::clamp(state.zoom * std::pow(1.15F, ImGui::GetIO().MouseWheel),
                                kMinimumZoom,
                                kMaximumZoom);
        const float ratio = oldZoom > 0.0F ? state.zoom / oldZoom : 1.0F;
        state.pan = {pointer.x - canvasMinimum.x
                         - (pointer.x - canvasMinimum.x - state.pan.x) * ratio,
                     pointer.y - canvasMinimum.y
                         - (pointer.y - canvasMinimum.y - state.pan.y) * ratio};
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
    for (float x = minimum.x + offsetX; x < maximum.x; x += grid) {
        drawList.AddLine({x, minimum.y}, {x, maximum.y}, kGrid, 1.0F);
    }
    for (float y = minimum.y + offsetY; y < maximum.y; y += grid) {
        drawList.AddLine({minimum.x, y}, {maximum.x, y}, kGrid, 1.0F);
    }
}

void draw_routed_edge(ImDrawList& drawList,
                      ImVec2 from,
                      ImVec2 to,
                      ImU32 color,
                      float thickness) noexcept {
    const float control = (std::max)(scaled(28.0F), std::abs(to.x - from.x) * 0.45F);
    drawList.AddBezierCubic(from,
                            {from.x + control, from.y},
                            {to.x - control, to.y},
                            to,
                            color,
                            thickness);
}

void draw_arrowhead(ImDrawList& drawList,
                    ImVec2 to,
                    bool pointsRight,
                    ImU32 color,
                    float thickness) noexcept {
    const float length = scaled(8.0F);
    const float half = scaled(4.0F);
    const float direction = pointsRight ? -1.0F : 1.0F;
    drawList.AddLine(to,
                     {to.x + direction * length, to.y - half},
                     color,
                     thickness);
    drawList.AddLine(to,
                     {to.x + direction * length, to.y + half},
                     color,
                     thickness);
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
    drawList.AddRectFilled(rect[0], rect[1], background, scaled(3.0F));
    drawList.AddRect(rect[0], rect[1], selected ? kSelection : kGrid, scaled(3.0F));
    drawList.AddRectFilled({rect[0].x, rect[0].y},
                           {rect[0].x + scaled(4.0F), rect[1].y},
                           kind_color(node.kind),
                           scaled(2.0F));
    if (zoom < 0.38F) {
        return;
    }
    const float textWidth = (std::max)(1.0F, rect[1].x - rect[0].x - scaled(18.0F));
    const std::string name = ellipsize(node.name, textWidth);
    drawList.AddText({rect[0].x + scaled(10.0F), rect[0].y + scaled(8.0F)},
                     kText,
                     name.c_str());
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
    drawList.AddText({rect[0].x + scaled(10.0F), rect[0].y + scaled(31.0F)},
                     kMuted,
                     detail.c_str());
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

[[nodiscard]] inspection::NodeId logic_node_for_definition(
    const inspection::Graph& graph,
    std::uint32_t definitionTag) noexcept {
    for (const inspection::Node& node : graph.nodes()) {
        if (node.kind == inspection::NodeKind::logicEntity
            && node.activityLogicMetadata.has_value()
            && node.activityLogicMetadata->definitionTag == definitionTag) {
            return node.id;
        }
    }
    return {};
}

[[nodiscard]] std::uint64_t relationship_revision(
    const inspection::ActivityLogicMetadata& metadata) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    const auto mix = [&value](std::uint64_t lane) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value ^= static_cast<std::uint8_t>(lane >> shift);
            value *= 1099511628211ULL;
        }
    };
    mix(metadata.definitionTag);
    for (const inspection::ActivityLogicRelationship& relationship : metadata.relationships) {
        mix(relationship.definitionTag);
        mix(relationship.nameHash);
        mix(relationship.occurrenceCount);
        mix(relationship.outgoing ? 1U : 0U);
    }
    return value;
}

} // namespace

void reset(State& state) noexcept {
    state = {};
    state.zoom = 1.0F;
    state.fitRequested = true;
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
        || state.cachedAdmissionRevision != admissionRevision || state.cachedRoot != root) {
        const bool firstLayout = state.layout.empty();
        const bool rootChanged = state.cachedRoot != root;
        graph_layout::compute(graph, root, admitted, state.layout);
        rebuild_index(state);
        state.cachedGeneration = graph.generation();
        state.cachedAdmissionRevision = admissionRevision;
        state.cachedRoot = root;
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
        if ((from.x < minimum.x && to.x < minimum.x)
            || (from.x > maximum.x && to.x > maximum.x)
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
        if (rect[1].x < minimum.x || rect[0].x > maximum.x
            || rect[1].y < minimum.y || rect[0].y > maximum.y) {
            continue;
        }
        if (inside(pointer, rect[0], rect[1])) {
            result.hovered = layout.id;
        }
        draw_card(*drawList,
                  *node,
                  rect,
                  layout.id == selected,
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

    if (!state.layout.empty()) {
        std::array<char, 192> status{};
        std::snprintf(status.data(),
                      status.size(),
                      "Ownership graph - %zu nodes, %zu omitted, %.0f%% zoom",
                      state.layout.size(),
                      omittedCount,
                      static_cast<double>(state.zoom * 100.0F));
        drawList->AddText({minimum.x + scaled(8.0F), minimum.y + scaled(8.0F)},
                          kMuted,
                          status.data());
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
    const inspection::Node* entity = logic_entity(graph, selected);
    if (entity == nullptr) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(minimum, maximum, kBackground);
        drawList->AddText({minimum.x + scaled(12.0F), minimum.y + scaled(12.0F)},
                          kMuted,
                          "Select an Activity Logic definition or authored placement.");
        return result;
    }

    const inspection::ActivityLogicMetadata& metadata = *entity->activityLogicMetadata;
    const std::uint64_t revision = relationship_revision(metadata);
    if (state.cachedGeneration != graph.generation()
        || state.cachedAdmissionRevision != revision || state.cachedRoot != entity->id) {
        const bool firstLayout = state.layout.empty();
        const bool rootChanged = state.cachedRoot != entity->id;
        state.layout.clear();
        state.layout.push_back(LayoutNode{entity->id, {0.0F, 0.0F}, 0});

        struct Target final {
            inspection::NodeId id{};
            std::uint32_t definitionTag{};
            bool incoming{};
            bool outgoing{};
        };
        std::vector<Target> targets;
        std::unordered_map<std::uint64_t, std::size_t> targetIndex;
        for (const inspection::ActivityLogicRelationship& relationship : metadata.relationships) {
            const inspection::NodeId id =
                logic_node_for_definition(graph, relationship.definitionTag);
            if (!id) {
                continue;
            }
            const auto [iterator, inserted] =
                targetIndex.emplace(id.value, targets.size());
            if (inserted) {
                targets.push_back(Target{id, relationship.definitionTag});
            }
            Target& target = targets[iterator->second];
            target.outgoing = target.outgoing || relationship.outgoing;
            target.incoming = target.incoming || !relationship.outgoing;
        }
        std::ranges::stable_sort(targets, [](const Target& left, const Target& right) {
            if (left.outgoing != right.outgoing) {
                return left.outgoing;
            }
            return left.definitionTag < right.definitionTag;
        });

        std::size_t incomingIndex = 0;
        std::size_t outgoingIndex = 0;
        const std::size_t incomingCount =
            static_cast<std::size_t>(std::ranges::count_if(targets, [](const Target& target) {
                return target.incoming && !target.outgoing;
            }));
        const std::size_t outgoingCount = targets.size() - incomingCount;
        const float column = scaled(310.0F);
        const float row = scaled(92.0F);
        for (const Target& target : targets) {
            const bool right = target.outgoing;
            const std::size_t index = right ? outgoingIndex++ : incomingIndex++;
            const std::size_t count = right ? outgoingCount : incomingCount;
            const float y = (static_cast<float>(index)
                             - (static_cast<float>((std::max)(count, std::size_t{1})) - 1.0F)
                                   * 0.5F)
                            * row;
            state.layout.push_back(
                LayoutNode{target.id, {right ? column : -column, y}, 1});
        }
        rebuild_index(state);
        state.cachedGeneration = graph.generation();
        state.cachedAdmissionRevision = revision;
        state.cachedRoot = entity->id;
        state.fitRequested = state.fitRequested || firstLayout || rootChanged;
    }

    handle_navigation(state, minimum, size, hoveredCanvas);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    draw_background(*drawList, minimum, maximum, state);
    drawList->PushClipRect(minimum, maximum, true);

    std::size_t unresolved = 0;
    const LayoutNode* entityLayout = layout_for(state, entity->id);
    if (entityLayout != nullptr) {
        const auto entityRect = card_rect(minimum, state, *entityLayout);
        for (const inspection::ActivityLogicRelationship& relationship : metadata.relationships) {
            const inspection::NodeId targetId =
                logic_node_for_definition(graph, relationship.definitionTag);
            const LayoutNode* targetLayout = layout_for(state, targetId);
            if (targetLayout == nullptr) {
                ++unresolved;
                continue;
            }
            const auto targetRect = card_rect(minimum, state, *targetLayout);
            ImVec2 from{};
            ImVec2 to{};
            if (relationship.outgoing) {
                from = {entityRect[1].x, (entityRect[0].y + entityRect[1].y) * 0.5F};
                to = {targetRect[0].x, (targetRect[0].y + targetRect[1].y) * 0.5F};
            } else {
                from = {targetRect[1].x, (targetRect[0].y + targetRect[1].y) * 0.5F};
                to = {entityRect[0].x, (entityRect[0].y + entityRect[1].y) * 0.5F};
            }
            const ImU32 color =
                relationship.outgoing ? kRelationshipOutgoing : kRelationshipIncoming;
            draw_routed_edge(*drawList, from, to, color, scaled(1.8F));
            draw_arrowhead(*drawList,
                           to,
                           relationship.outgoing,
                           color,
                           scaled(1.6F));
            if (state.zoom >= 0.68F) {
                std::array<char, 80> label{};
                std::snprintf(label.data(),
                              label.size(),
                              "0x%08X x%u",
                              relationship.nameHash,
                              relationship.occurrenceCount);
                drawList->AddText({(from.x + to.x) * 0.5F + scaled(4.0F),
                                   (from.y + to.y) * 0.5F - scaled(10.0F)},
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
                  layout.id == entity->id,
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
                  "Serialized Activity Logic relationships - %zu links, %zu unresolved; static catalog evidence",
                  metadata.relationships.size(),
                  unresolved);
    drawList->AddText({minimum.x + scaled(8.0F), minimum.y + scaled(8.0F)},
                      kMuted,
                      status.data());
    return result;
}

} // namespace sunrise::client::ui::world_inspector::graph
