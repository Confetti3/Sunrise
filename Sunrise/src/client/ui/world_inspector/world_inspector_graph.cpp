#include "world_inspector_graph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <unordered_map>

#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::client::ui::world_inspector::graph {
namespace {

namespace dpi = core::ui::scaling::dpi;

constexpr float kCardWidth = 190.0F;
constexpr float kCardHeight = 58.0F;
constexpr float kHorizontalGap = 78.0F;
constexpr float kVerticalGap = 26.0F;
constexpr float kMinimumZoom = 0.25F;
constexpr float kMaximumZoom = 2.5F;
constexpr ImU32 kBackground = IM_COL32(11, 12, 17, 255);
constexpr ImU32 kGrid = IM_COL32(32, 34, 43, 170);
constexpr ImU32 kCard = IM_COL32(25, 27, 35, 245);
constexpr ImU32 kCardHovered = IM_COL32(42, 44, 56, 250);
constexpr ImU32 kCardSelected = IM_COL32(54, 48, 35, 250);
constexpr ImU32 kEdge = IM_COL32(119, 125, 141, 210);
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
    case inspection::NodeKind::light: return IM_COL32(255, 226, 117, 255);
    default: return IM_COL32(137, 146, 164, 255);
    }
}

[[nodiscard]] bool inside(ImVec2 point, ImVec2 minimum, ImVec2 maximum) noexcept {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y
           && point.y <= maximum.y;
}

[[nodiscard]] const LayoutNode* layout_for(const State& state, inspection::NodeId id) noexcept {
    const auto iterator = std::ranges::find_if(state.layout, [id](const LayoutNode& value) {
        return value.id == id;
    });
    return iterator == state.layout.end() ? nullptr : &*iterator;
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
                                   ImVec2 pan,
                                   float zoom,
                                   const LayoutNode& node) noexcept {
    return {canvasMinimum.x + pan.x + node.position[0] * zoom,
            canvasMinimum.y + pan.y + node.position[1] * zoom};
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
        graph_layout::compute(graph, root, admitted, state.layout);
        state.cachedGeneration = graph.generation();
        state.cachedAdmissionRevision = admissionRevision;
        state.cachedRoot = root;
        state.fitRequested = true;
    }
    if (state.fitRequested) {
        fit(state, size);
    }

    if (hoveredCanvas && ImGui::GetIO().MouseWheel != 0.0F) {
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const float oldZoom = state.zoom;
        state.zoom = std::clamp(state.zoom * std::pow(1.15F, ImGui::GetIO().MouseWheel),
                                kMinimumZoom,
                                kMaximumZoom);
        const float ratio = oldZoom > 0.0F ? state.zoom / oldZoom : 1.0F;
        state.pan = {pointer.x - minimum.x - (pointer.x - minimum.x - state.pan.x) * ratio,
                     pointer.y - minimum.y - (pointer.y - minimum.y - state.pan.y) * ratio};
    }
    if (hoveredCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        state.pan.x += ImGui::GetIO().MouseDelta.x;
        state.pan.y += ImGui::GetIO().MouseDelta.y;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(minimum, maximum, kBackground);
    const float grid = scaled(48.0F) * state.zoom;
    if (grid > 4.0F) {
        const float offsetX = std::fmod(state.pan.x, grid);
        const float offsetY = std::fmod(state.pan.y, grid);
        for (float x = minimum.x + offsetX; x < maximum.x; x += grid) {
            drawList->AddLine({x, minimum.y}, {x, maximum.y}, kGrid, 1.0F);
        }
        for (float y = minimum.y + offsetY; y < maximum.y; y += grid) {
            drawList->AddLine({minimum.x, y}, {maximum.x, y}, kGrid, 1.0F);
        }
    }
    drawList->PushClipRect(minimum, maximum, true);
    const float cardWidth = scaled(kCardWidth) * state.zoom;
    const float cardHeight = scaled(kCardHeight) * state.zoom;
    const auto card_rect = [&](const LayoutNode& node) {
        const ImVec2 center = screen_center(minimum, state.pan, state.zoom, node);
        return std::array<ImVec2, 2>{{{center.x - cardWidth * 0.5F, center.y - cardHeight * 0.5F},
                                      {center.x + cardWidth * 0.5F, center.y + cardHeight * 0.5F}}};
    };

    for (const LayoutNode& layout : state.layout) {
        const inspection::Node* node = graph.node(layout.id);
        if (node == nullptr || !node->parent || !admitted.contains(node->parent.value)) {
            continue;
        }
        const LayoutNode* parent = layout_for(state, node->parent);
        if (parent == nullptr) {
            continue;
        }
        const ImVec2 from = screen_center(minimum, state.pan, state.zoom, *parent);
        const ImVec2 to = screen_center(minimum, state.pan, state.zoom, layout);
        if ((from.x < minimum.x && to.x < minimum.x) || (from.x > maximum.x && to.x > maximum.x)
            || (from.y < minimum.y && to.y < minimum.y)
            || (from.y > maximum.y && to.y > maximum.y)) {
            continue;
        }
        drawList->AddLine(from, to, kEdge, scaled(1.5F));
    }

    const ImVec2 pointer = ImGui::GetIO().MousePos;
    for (const LayoutNode& layout : state.layout) {
        const inspection::Node* node = graph.node(layout.id);
        if (node == nullptr) {
            continue;
        }
        const auto rect = card_rect(layout);
        if (inside(pointer, rect[0], rect[1])) {
            result.hovered = layout.id;
        }
        const bool isSelected = layout.id == selected;
        const bool isHovered = layout.id == result.hovered;
        const ImU32 background = isSelected ? kCardSelected : (isHovered ? kCardHovered : kCard);
        drawList->AddRectFilled(rect[0], rect[1], background, scaled(3.0F));
        drawList->AddRect(rect[0], rect[1], isSelected ? kSelection : kGrid, scaled(3.0F));
        drawList->AddRectFilled({rect[0].x, rect[0].y},
                                {rect[0].x + scaled(4.0F), rect[1].y},
                                kind_color(node->kind),
                                scaled(2.0F));
        const float textScale = state.zoom < 0.55F ? 0.0F : 1.0F;
        if (textScale != 0.0F) {
            drawList->AddText({rect[0].x + scaled(10.0F), rect[0].y + scaled(8.0F)},
                              kText,
                              node->name.c_str());
            std::array<char, 128> status{};
            std::snprintf(status.data(),
                          status.size(),
                          "%s / %s",
                          inspection::status_name(node->status),
                          inspection::provenance_name(node->provenance));
            drawList->AddText({rect[0].x + scaled(10.0F), rect[0].y + scaled(31.0F)},
                              kMuted,
                              status.data());
        }
    }
    drawList->PopClipRect();

    if (hoveredCanvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (result.hovered) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                result.focused = result.hovered;
            } else {
                result.selected = result.hovered;
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
        drawList->AddText({minimum.x + scaled(8.0F), minimum.y + scaled(8.0F)},
                          kMuted,
                          "Ownership graph - drag middle mouse, wheel to zoom");
    } else {
        drawList->AddText({minimum.x + scaled(12.0F), minimum.y + scaled(12.0F)},
                          kMuted,
                          "No admitted nodes in the current filter.");
    }
    return result;
}

} // namespace sunrise::client::ui::world_inspector::graph
