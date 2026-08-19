#include "world_inspector_viewport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace sunrise::client::ui::world_inspector::viewport {
namespace {

constexpr float kNearDepth = 0.01F;
constexpr float kPickRadius = 9.0F;
constexpr float kMarkerRadius = 4.5F;
constexpr float kSelectedRadius = 10.0F;
constexpr ImU32 kSpawnColor = IM_COL32(230, 184, 74, 235);
constexpr ImU32 kSelectionColor = IM_COL32(66, 184, 231, 255);
constexpr ImU32 kHoverColor = IM_COL32(151, 220, 246, 255);
constexpr ImU32 kHiddenColor = IM_COL32(116, 124, 135, 230);
constexpr ImU32 kLabelBackground = IM_COL32(15, 18, 22, 225);

struct Projected final {
    inspection::NodeId id{};
    ImVec2 screen{};
    float depth{};
    bool hidden{};
};

[[nodiscard]] float dot(const std::array<float, 3>& left,
                        const std::array<float, 3>& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] std::array<float, 3> cross(const std::array<float, 3>& left,
                                         const std::array<float, 3>& right) noexcept {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] bool finite(const std::array<float, 3>& value) noexcept {
    return std::ranges::all_of(value, [](float lane) { return std::isfinite(lane); });
}

[[nodiscard]] bool project(const inspection::Transform& transform,
                           const client::viewer::camera::Pose& pose,
                           const ImVec2& projectionPosition,
                           const ImVec2& projectionSize,
                           ImVec2& screen,
                           float& depth) noexcept {
    if (!finite(transform.position) || !finite(pose.position) || !finite(pose.forward)
        || !finite(pose.up) || !std::isfinite(pose.fov) || pose.fov <= 0.05F
        || pose.fov >= 179.0F || projectionSize.x <= 0.0F || projectionSize.y <= 0.0F) {
        return false;
    }

    const std::array<float, 3> relative{
        transform.position[0] - pose.position[0],
        transform.position[1] - pose.position[1],
        transform.position[2] - pose.position[2],
    };
    const std::array<float, 3> right = cross(pose.forward, pose.up);
    depth = dot(relative, pose.forward);
    if (!std::isfinite(depth) || depth <= kNearDepth) {
        return false;
    }

    constexpr float kPi = 3.14159265358979323846F;
    const float halfFov = pose.fov <= kPi + 0.1F ? pose.fov * 0.5F
                                                 : pose.fov * (kPi / 360.0F);
    const float horizontalTangent = std::tan(halfFov);
    const float aspect = projectionSize.x / projectionSize.y;
    const float verticalTangent = horizontalTangent / aspect;
    if (!std::isfinite(horizontalTangent) || horizontalTangent <= 0.0F
        || !std::isfinite(verticalTangent) || verticalTangent <= 0.0F
        || !std::isfinite(aspect) || aspect <= 0.0F) {
        return false;
    }
    const float horizontal = dot(relative, right) / (depth * horizontalTangent);
    const float vertical = dot(relative, pose.up) / (depth * verticalTangent);
    if (!std::isfinite(horizontal) || !std::isfinite(vertical)) {
        return false;
    }

    screen.x = projectionPosition.x + (horizontal * 0.5F + 0.5F) * projectionSize.x;
    screen.y = projectionPosition.y + (0.5F - vertical * 0.5F) * projectionSize.y;
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

[[nodiscard]] bool inside(const ImVec2& point, const ImVec2& minimum, const ImVec2& maximum) noexcept {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y
           && point.y <= maximum.y;
}

void fit_frame(const client::hooks::graphics::renderer::frame_capture::View& frame,
               const ImVec2& areaMinimum,
               const ImVec2& areaMaximum,
               ImVec2& imageMinimum,
               ImVec2& imageMaximum) noexcept {
    imageMinimum = areaMinimum;
    imageMaximum = areaMaximum;
    if (!frame) {
        return;
    }
    const float areaWidth = areaMaximum.x - areaMinimum.x;
    const float areaHeight = areaMaximum.y - areaMinimum.y;
    const float sourceAspect = frame.width / frame.height;
    const float areaAspect = areaWidth / areaHeight;
    float width = areaWidth;
    float height = areaHeight;
    if (areaAspect > sourceAspect) {
        width = areaHeight * sourceAspect;
    } else {
        height = areaWidth / sourceAspect;
    }
    imageMinimum = {areaMinimum.x + (areaWidth - width) * 0.5F,
                    areaMinimum.y + (areaHeight - height) * 0.5F};
    imageMaximum = {imageMinimum.x + width, imageMinimum.y + height};
}

void draw_marker(ImDrawList& drawList,
                 const Projected& projected,
                 bool selected,
                 bool hovered) noexcept {
    const ImU32 color = projected.hidden ? kHiddenColor
                         : selected       ? kSelectionColor
                         : hovered        ? kHoverColor
                                          : kSpawnColor;
    const float radius = selected ? kSelectedRadius : (hovered ? 7.0F : kMarkerRadius);
    if (selected) {
        drawList.AddCircle(projected.screen, radius, color, 20, 2.0F);
        drawList.AddLine({projected.screen.x - radius - 3.0F, projected.screen.y},
                         {projected.screen.x + radius + 3.0F, projected.screen.y},
                         color,
                         1.5F);
        drawList.AddLine({projected.screen.x, projected.screen.y - radius - 3.0F},
                         {projected.screen.x, projected.screen.y + radius + 3.0F},
                         color,
                         1.5F);
    } else {
        const std::array<ImVec2, 4> diamond{
            ImVec2{projected.screen.x, projected.screen.y - radius},
            ImVec2{projected.screen.x + radius, projected.screen.y},
            ImVec2{projected.screen.x, projected.screen.y + radius},
            ImVec2{projected.screen.x - radius, projected.screen.y},
        };
        drawList.AddConvexPolyFilled(diamond.data(), static_cast<int>(diamond.size()), color);
    }
}

void draw_label(ImDrawList& drawList,
                const inspection::Node& node,
                const Projected& projected,
                ImU32 color) noexcept {
    const ImVec2 textSize = ImGui::CalcTextSize(node.name.c_str());
    const ImVec2 minimum{projected.screen.x + 12.0F, projected.screen.y - textSize.y * 0.5F - 3.0F};
    const ImVec2 maximum{minimum.x + textSize.x + 8.0F, minimum.y + textSize.y + 6.0F};
    drawList.AddRectFilled(minimum, maximum, kLabelBackground, 2.0F);
    drawList.AddRect(minimum, maximum, color, 2.0F);
    drawList.AddText({minimum.x + 4.0F, minimum.y + 3.0F}, color, node.name.c_str());
}

} // namespace

Result draw(const inspection::Graph& graph,
            inspection::NodeId selected,
            const std::unordered_set<std::uint64_t>& hidden,
            const client::viewer::camera::Status& camera,
            const client::hooks::graphics::renderer::frame_capture::View& frame,
            const Options& options,
            bool navigationActive) noexcept {
    Result result{};
    result.navigation = navigationActive;

    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 1.0F);
    size.y = (std::max)(size.y, 1.0F);
    const ImVec2 areaMinimum = ImGui::GetCursorScreenPos();
    const ImVec2 areaMaximum{areaMinimum.x + size.x, areaMinimum.y + size.y};
    ImVec2 imageMinimum{};
    ImVec2 imageMaximum{};
    fit_frame(frame, areaMinimum, areaMaximum, imageMinimum, imageMaximum);
    ImGui::InvisibleButton("##world_inspector_viewport",
                           size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    const bool viewportHovered =
        ImGui::IsItemHovered() && inside(pointer, imageMinimum, imageMaximum);
    if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        result.navigation = camera.active;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || !camera.active
        || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        result.navigation = false;
    }

    ImVec2 projectionPosition = imageMinimum;
    ImVec2 projectionSize{imageMaximum.x - imageMinimum.x, imageMaximum.y - imageMinimum.y};
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (!frame && mainViewport != nullptr) {
        projectionPosition = mainViewport->Pos;
        projectionSize = mainViewport->Size;
    }

    static std::vector<Projected> projected;
    projected.clear();
    if (camera.active) {
        for (const inspection::Node& node : graph.nodes()) {
            if (node.kind != inspection::NodeKind::spawnPoint || !node.transform.has_value()) {
                continue;
            }
            const bool selectedNode = node.id == selected;
            const bool hiddenNode = hidden.contains(node.id.value);
            if ((!options.showSpawns || hiddenNode) && !selectedNode) {
                continue;
            }
            Projected marker{node.id, {}, 0.0F, hiddenNode};
            if (project(*node.transform,
                        camera.pose,
                        projectionPosition,
                        projectionSize,
                        marker.screen,
                        marker.depth)
                && inside(marker.screen, imageMinimum, imageMaximum)) {
                projected.push_back(marker);
            }
        }
    }

    float closestDistance = (std::numeric_limits<float>::max)();
    float closestDepth = (std::numeric_limits<float>::max)();
    if (viewportHovered) {
        for (const Projected& marker : projected) {
            if (marker.hidden) {
                continue;
            }
            const float x = pointer.x - marker.screen.x;
            const float y = pointer.y - marker.screen.y;
            const float distance = x * x + y * y;
            if (distance > kPickRadius * kPickRadius) {
                continue;
            }
            if (distance < closestDistance
                || (distance == closestDistance && marker.depth < closestDepth)
                || (distance == closestDistance && marker.depth == closestDepth
                    && marker.id.value < result.hovered.value)) {
                result.hovered = marker.id;
                closestDistance = distance;
                closestDepth = marker.depth;
            }
        }
    }

    if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            result.focused = result.hovered;
        } else {
            result.selected = result.hovered;
        }
    }
    if (viewportHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)
        && result.hovered) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        if (drag.x * drag.x + drag.y * drag.y <= 16.0F) {
            result.context = result.hovered;
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (frame) {
        drawList->AddImage(frame.texture, imageMinimum, imageMaximum);
    }
    drawList->PushClipRect(imageMinimum, imageMaximum, true);
    if (!camera.active) {
        const char* message = "Enable Viewer Camera to align and pick world helpers";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        const float imageWidth = imageMaximum.x - imageMinimum.x;
        const ImVec2 textPosition{imageMinimum.x + (imageWidth - textSize.x) * 0.5F,
                                  imageMinimum.y + 12.0F};
        drawList->AddRectFilled({textPosition.x - 8.0F, textPosition.y - 5.0F},
                                {textPosition.x + textSize.x + 8.0F,
                                 textPosition.y + textSize.y + 5.0F},
                                kLabelBackground,
                                2.0F);
        drawList->AddText(textPosition, kHiddenColor, message);
    }
    for (const Projected& marker : projected) {
        const bool selectedNode = marker.id == selected;
        const bool hoveredNode = marker.id == result.hovered;
        draw_marker(*drawList, marker, selectedNode, hoveredNode);
        if (selectedNode || hoveredNode || options.showLabels) {
            const inspection::Node* node = graph.node(marker.id);
            if (node != nullptr) {
                const ImU32 color = marker.hidden ? kHiddenColor
                                    : selectedNode ? kSelectionColor
                                    : hoveredNode  ? kHoverColor
                                                   : kSpawnColor;
                draw_label(*drawList, *node, marker, color);
            }
        }
    }
    drawList->PopClipRect();

    if (result.hovered) {
        const inspection::Node* node = graph.node(result.hovered);
        if (node != nullptr && node->transform.has_value()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(node->name.c_str());
            ImGui::TextDisabled("%s", inspection::kind_name(node->kind));
            ImGui::Separator();
            const auto& position = node->transform->position;
            ImGui::Text("Position  %.3f  %.3f  %.3f",
                        static_cast<double>(position[0]),
                        static_cast<double>(position[1]),
                        static_cast<double>(position[2]));
            ImGui::TextDisabled("Bounds unavailable");
            ImGui::EndTooltip();
        }
    }

    return result;
}

} // namespace sunrise::client::ui::world_inspector::viewport
