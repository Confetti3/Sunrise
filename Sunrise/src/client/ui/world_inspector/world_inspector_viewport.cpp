#include "world_inspector_viewport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui_internal.h>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "world_inspector_label_layout.h"

namespace sunrise::client::ui::world_inspector::viewport {
namespace {

namespace dpi = core::ui::scaling::dpi;

template <typename Measure>
[[nodiscard]] std::string fit_text(std::string_view text, float maximumWidth, Measure&& measure) {
    if (text.empty() || maximumWidth <= 0.0F) {
        return {};
    }
    if (measure(text) <= maximumWidth) {
        return std::string(text);
    }
    constexpr std::string_view ellipsis = "...";
    if (measure(ellipsis) > maximumWidth) {
        return {};
    }
    for (std::size_t length = text.size(); length != 0; --length) {
        std::string_view prefix = text.substr(0, length);
        while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '|')) {
            prefix.remove_suffix(1);
        }
        std::string candidate(prefix);
        candidate.append(ellipsis);
        if (measure(candidate) <= maximumWidth) {
            return candidate;
        }
    }
    return std::string(ellipsis);
}

constexpr ImU32 kHoverColor = IM_COL32(255, 210, 63, 255);
constexpr ImU32 kHiddenColor = IM_COL32(116, 124, 135, 230);
constexpr ImU32 kDisclosureColor = IM_COL32(204, 210, 218, 245);
constexpr ImU32 kLabelBackground = IM_COL32(31, 39, 27, 225);
struct Projected final {
    inspection::NodeId id{};
    inspection::NodeKind kind{inspection::NodeKind::unresolved};
    ImVec2 screen{};
    float depth{};
    ImU32 plannedColor{};
};

struct ProjectedPoint final {
    ImVec2 screen{};
    float depth{};
};

struct LabelPlacement final {
    inspection::NodeId nodeId{};
    Projected projected{};
    ImU32 color{};
    ImVec2 minimum{};
    ImVec2 maximum{};
    float desiredY{};
    int priority{};
    std::array<char, 64> detail{};
};

struct ViewportState final {
    std::vector<Projected> projected;
    std::vector<LabelPlacement> labels;
    std::vector<label_layout::Candidate> labelCandidates;
    std::vector<label_layout::Placement> labelResults;
    Result lastComplete{};
    bool hasLastComplete{};
    std::uint64_t pickSequence{};
    inspection::PickRequest pendingPick{};
    ImVec2 pendingPickPointer{};
    ImVec2 gpuPickPointer{};
    inspection::NodeId gpuHovered{};
    bool gpuPickValid{};
};

[[nodiscard]] ViewportState& state() noexcept {
    static ViewportState value;
    return value;
}

[[nodiscard]] Result skipped_result(const Result& current, bool reservationFailure) noexcept {
    ViewportState& retained = state();
    Result result = retained.hasLastComplete ? retained.lastComplete : current;
    // Interaction/navigation belongs to the current frame even when its
    // geometry transaction is skipped. All expensive projection/label counts
    // remain those of the last complete frame for stable diagnostics.
    result.hovered = current.hovered;
    result.selected = current.selected;
    result.focused = current.focused;
    result.context = current.context;
    result.navigation = current.navigation;
    result.clearSelection = current.clearSelection;
    result.allocationFailure = true;
    result.reservationFailure = reservationFailure;
    if (reservationFailure) {
        result.plannedVertices = current.plannedVertices;
        result.plannedIndices = current.plannedIndices;
    }
    return result;
}

[[nodiscard]] ImU32 helper_color(const std::array<float, 4>& color) noexcept {
    const auto lane = [](float value) noexcept {
        return static_cast<int>((std::clamp)(value, 0.0F, 1.0F) * 255.0F + 0.5F);
    };
    return IM_COL32(lane(color[0]), lane(color[1]), lane(color[2]), lane(color[3]));
}

[[nodiscard]] ImU32 helper_color(inspection::NodeKind kind) noexcept {
    inspection::HelperGlyph glyph = inspection::HelperGlyph::spawn;
    switch (kind) {
    case inspection::NodeKind::geometry:
        glyph = inspection::HelperGlyph::geometry;
        break;
    case inspection::NodeKind::runtimeEntity:
        glyph = inspection::HelperGlyph::runtimeEntity;
        break;
    case inspection::NodeKind::trigger:
        glyph = inspection::HelperGlyph::trigger;
        break;
    case inspection::NodeKind::audio:
        glyph = inspection::HelperGlyph::audio;
        break;
    case inspection::NodeKind::physics:
        glyph = inspection::HelperGlyph::physics;
        break;
    case inspection::NodeKind::logicPlacement:
        glyph = inspection::HelperGlyph::action;
        break;
    default:
        break;
    }
    return helper_color(inspection::helper_role_color(glyph));
}

[[nodiscard]] float scaled(float value) noexcept {
    return dpi::pixels(value);
}

[[nodiscard]] bool
inside(const ImVec2& point, const ImVec2& minimum, const ImVec2& maximum) noexcept {
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

[[nodiscard]] LabelPlacement make_label(const inspection::Node& node,
                                        const Projected& projected,
                                        ImU32 color,
                                        const ImVec2& imageMinimum,
                                        const ImVec2& imageMaximum,
                                        int priority) noexcept {
    std::array<char, 64> detail{};
    if (priority > 0 && node.bounds.has_value() && inspection::bounds_valid(*node.bounds)) {
        const inspection::Bounds& bounds = *node.bounds;
        std::snprintf(detail.data(),
                      detail.size(),
                      "%.2f x %.2f x %.2f m",
                      static_cast<double>(bounds.maximum[0] - bounds.minimum[0]),
                      static_cast<double>(bounds.maximum[1] - bounds.minimum[1]),
                      static_cast<double>(bounds.maximum[2] - bounds.minimum[2]));
    }
    const ImVec2 textSize = ImGui::CalcTextSize(node.name.c_str());
    const ImVec2 detailSize = detail[0] != '\0' ? ImGui::CalcTextSize(detail.data()) : ImVec2{};
    const float width = (std::max)(textSize.x, detailSize.x) + scaled(8.0F);
    const float height = textSize.y + (detail[0] != '\0' ? detailSize.y : 0.0F) + scaled(6.0F);
    float x = projected.screen.x + scaled(12.0F);
    if (x + width > imageMaximum.x) {
        x = projected.screen.x - scaled(12.0F) - width;
    }
    x = std::clamp(x, imageMinimum.x, (std::max)(imageMinimum.x, imageMaximum.x - width));
    const float desiredY = projected.screen.y - height * 0.5F;
    const float y =
        std::clamp(desiredY, imageMinimum.y, (std::max)(imageMinimum.y, imageMaximum.y - height));
    LabelPlacement result{};
    result.nodeId = node.id;
    result.projected = projected;
    result.color = color;
    result.minimum = {x, y};
    result.maximum = {x + width, y + height};
    result.desiredY = desiredY;
    result.priority = priority;
    result.detail = detail;
    return result;
}

[[nodiscard]] bool project_exact(const inspection::RenderViewSnapshot& view,
                                 const std::array<float, 3>& point,
                                 const ImVec2& imageMinimum,
                                 const ImVec2& imageMaximum,
                                 ProjectedPoint& projected) noexcept {
    if (!view.valid || !view.exactNative || view.viewport.width <= 0.0F
        || view.viewport.height <= 0.0F) {
        return false;
    }
    const auto& m = view.viewProjection;
    const float x = point[0];
    const float y = point[1];
    const float z = point[2];
    const float clipX = m[0] * x + m[1] * y + m[2] * z + m[3];
    const float clipY = m[4] * x + m[5] * y + m[6] * z + m[7];
    const float clipZ = m[8] * x + m[9] * y + m[10] * z + m[11];
    const float clipW = m[12] * x + m[13] * y + m[14] * z + m[15];
    if (!std::isfinite(clipW) || clipW <= 1.0e-5F) {
        return false;
    }
    const float normalizedX = clipX / clipW * 0.5F + 0.5F;
    const float normalizedY = -clipY / clipW * 0.5F + 0.5F;
    if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY)) {
        return false;
    }
    projected.screen = {imageMinimum.x + normalizedX * (imageMaximum.x - imageMinimum.x),
                        imageMinimum.y + normalizedY * (imageMaximum.y - imageMinimum.y)};
    projected.depth = clipZ / clipW;
    return std::isfinite(projected.depth);
}

void draw_label(ImDrawList& drawList,
                const LabelPlacement& label,
                const inspection::Graph& graph) noexcept {
    const inspection::Node* node = graph.node(label.nodeId);
    if (node == nullptr) {
        return;
    }
    drawList.AddRectFilled(label.minimum, label.maximum, kLabelBackground, scaled(2.0F));
    drawList.AddRect(label.minimum, label.maximum, label.color, scaled(2.0F));
    drawList.AddText({label.minimum.x + scaled(4.0F), label.minimum.y + scaled(3.0F)},
                     label.color,
                     node->name.c_str());
    if (label.detail[0] != '\0') {
        drawList.AddText(
            {label.minimum.x + scaled(4.0F), label.minimum.y + scaled(3.0F) + ImGui::GetFontSize()},
            kHiddenColor,
            label.detail.data());
    }
}

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right, std::size_t& output) noexcept {
    if (right > (std::numeric_limits<std::size_t>::max)() - left) {
        return false;
    }
    output = left + right;
    return true;
}

[[nodiscard]] bool checked_mul(std::size_t left, std::size_t right, std::size_t& output) noexcept {
    if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
        return false;
    }
    output = left * right;
    return true;
}

/**
 * ImVector::reserve() assumes its allocator cannot return null.  The Sunrise
 * allocator is intentionally allowed to reject a request, so reserve through
 * this checked path before submitting any viewport geometry.  The old storage
 * is not touched until the replacement has been allocated and copied.
 */
template <typename T>
[[nodiscard]] bool reserve_checked(ImVector<T>& vector, std::size_t required) noexcept {
    if (required <= static_cast<std::size_t>((std::max)(vector.Capacity, 0))) {
        return true;
    }
    if (required > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    std::size_t grown = static_cast<std::size_t>((std::max)(vector.Capacity, 8));
    if (grown > static_cast<std::size_t>((std::numeric_limits<int>::max)()) / 2U) {
        grown = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    } else {
        grown *= 2U;
    }
    const std::size_t capacity = (std::max)(required, grown);
    if (capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const std::size_t bytes = capacity * sizeof(T);
    void* replacement = ImGui::MemAlloc(bytes);
    if (replacement == nullptr) {
        return false;
    }
    if (vector.Data != nullptr && vector.Size > 0) {
        std::memcpy(replacement, vector.Data, static_cast<std::size_t>(vector.Size) * sizeof(T));
    }
    ImGui::MemFree(vector.Data);
    vector.Data = static_cast<T*>(replacement);
    vector.Capacity = static_cast<int>(capacity);
    return true;
}

struct DrawPlan final {
    std::size_t vertices{};
    std::size_t indices{};
    std::size_t commands{};
    std::size_t path{};
    std::size_t temporary{};
    std::size_t textBytes{};
    bool valid{true};
};

[[nodiscard]] DrawPlan make_draw_plan(const inspection::Graph& graph,
                                      const std::vector<LabelPlacement>& labels,
                                      const std::vector<label_layout::Placement>& placed,
                                      bool frame,
                                      bool waiting) noexcept {
    DrawPlan plan{};
    auto add = [&plan](std::size_t& target, std::size_t amount) noexcept {
        if (!plan.valid || !checked_add(target, amount, target)) {
            plan.valid = false;
        }
    };
    auto add_primitive = [&add, &plan](std::size_t vertices, std::size_t indices) noexcept {
        add(plan.vertices, vertices);
        add(plan.indices, indices);
    };
    auto scaled_count =
        [&plan](std::size_t value, std::size_t factor, std::size_t& output) noexcept {
            if (!checked_mul(value, factor, output)) {
                plan.valid = false;
                output = 0U;
            }
        };
    if (frame) {
        add_primitive(4U, 6U);
    }
    // Footer eligibility can change while commit discovers a stale edge/node;
    // reserve its worst case unconditionally so that conditional footer
    // emission cannot allocate after the transaction begins.
    add_primitive(64U + 256U * 6U, 96U + 256U * 18U);
    add(plan.textBytes, 256U);
    if (waiting) {
        add_primitive(64U + 128U * 6U, 96U + 128U * 18U);
        add(plan.textBytes, 128U);
    } else {
        add_primitive(128U, 128U * 18U);
        add(plan.textBytes, 128U);
    }
    for (const label_layout::Placement& placement : placed) {
        if (placement.candidate >= labels.size()) {
            continue;
        }
        const LabelPlacement& label = labels[placement.candidate];
        const inspection::Node* node = graph.node(label.nodeId);
        std::size_t bytes = 0U;
        if (node != nullptr && !checked_add(node->name.size(), 1U, bytes)) {
            plan.valid = false;
        }
        if (label.detail[0] != '\0') {
            std::size_t detailed = 0U;
            if (!checked_add(std::strlen(label.detail.data()), 1U, detailed)
                || !checked_add(bytes, detailed, bytes)) {
                plan.valid = false;
            }
        }
        std::size_t textVertices = 0U;
        std::size_t textIndices = 0U;
        scaled_count(bytes, 6U, textVertices);
        scaled_count(bytes, 18U, textIndices);
        std::size_t vertices = 0U;
        std::size_t indices = 0U;
        if (!checked_add(128U, textVertices, vertices)
            || !checked_add(256U, textIndices, indices)) {
            plan.valid = false;
        }
        add_primitive(vertices, indices);
        add(plan.textBytes, bytes);
    }
    // Commands are shared while clip/texture headers match. Remaining growth
    // is 16-bit vertex-offset splitting at a safe 60k vertex chunk, plus a
    // small allowance for image/text transitions.
    constexpr std::size_t kSafeVertexChunk = 60'000U;
    std::size_t chunks = plan.vertices / kSafeVertexChunk;
    if (plan.vertices % kSafeVertexChunk != 0U && !checked_add(chunks, 1U, chunks)) {
        plan.valid = false;
    }
    std::size_t commands = 0U;
    if (!checked_add(chunks, 16U, commands)) {
        plan.valid = false;
    }
    add(plan.commands, commands);
    plan.path = 128U;
    plan.temporary = 128U;
    return plan;
}

[[nodiscard]] bool reserve_draw_list(ImDrawList& drawList, const DrawPlan& plan) noexcept {
    if (!plan.valid) {
        return false;
    }
    auto required = [](int size, std::size_t additional, std::size_t& output) noexcept {
        return checked_add(static_cast<std::size_t>((std::max)(size, 0)), additional, output);
    };
    std::size_t count = 0U;
    if (!required(drawList.VtxBuffer.Size, plan.vertices, count)
        || !reserve_checked(drawList.VtxBuffer, count)) {
        return false;
    }
    // Repair immediately. A later vector can fail (for example an injected
    // MemAlloc null), and leaving this pointer aimed at the freed old storage
    // would make the draw list invalid even though no primitive was emitted.
    drawList._VtxWritePtr = drawList.VtxBuffer.Data == nullptr
                                ? nullptr
                                : drawList.VtxBuffer.Data + drawList.VtxBuffer.Size;
    if (!required(drawList.IdxBuffer.Size, plan.indices, count)
        || !reserve_checked(drawList.IdxBuffer, count)) {
        return false;
    }
    drawList._IdxWritePtr = drawList.IdxBuffer.Data == nullptr
                                ? nullptr
                                : drawList.IdxBuffer.Data + drawList.IdxBuffer.Size;
    if (!required(drawList.CmdBuffer.Size, plan.commands, count)
        || !reserve_checked(drawList.CmdBuffer, count)
        || !required(drawList._Path.Size, plan.path, count)
        || !reserve_checked(drawList._Path, count)) {
        return false;
    }
    if (drawList._Data != nullptr) {
        if (!required(drawList._Data->TempBuffer.Size, plan.temporary, count)
            || !reserve_checked(drawList._Data->TempBuffer, count)) {
            return false;
        }
    }
    // PushClipRect()/AddImage() also append to these internal stacks.  Keep
    // those appends allocation-free during commit as well.
    if (!required(drawList._ClipRectStack.Size, 4U, count)
        || !reserve_checked(drawList._ClipRectStack, count)
        || !required(drawList._TextureStack.Size, 2U, count)
        || !reserve_checked(drawList._TextureStack, count)) {
        return false;
    }
    return true;
}

void draw_bounds_tooltip(const inspection::Bounds& bounds) noexcept {
    ImGui::Text("Bounds min %.3f  %.3f  %.3f",
                static_cast<double>(bounds.minimum[0]),
                static_cast<double>(bounds.minimum[1]),
                static_cast<double>(bounds.minimum[2]));
    ImGui::Text("Bounds max %.3f  %.3f  %.3f",
                static_cast<double>(bounds.maximum[0]),
                static_cast<double>(bounds.maximum[1]),
                static_cast<double>(bounds.maximum[2]));
}

} // namespace

inspection::OverlayPolicy overlay_policy(const Options& options) noexcept {
    return {options.showGeometry,
            options.showEntities,
            options.showSpawns,
            options.showLogic,
            options.showTriggers,
            options.showAudio,
            options.showLabels,
            options.showKnownBounds,
            options.showTriggerCenters,
            options.showAuthoredOrientation,
            options.detail,
            options.maximumVisibleNodes,
            options.nearbyRadius,
            options.glyphSizePixels,
            options.lineWidthPixels,
            options.baseOpacity,
            options.focusContextOpacity};
}

Result draw_impl(const inspection::Graph& graph,
                 inspection::NodeId selected,
                 const client::viewer::camera::Status& camera,
                 const client::hooks::graphics::renderer::frame_capture::View& frame,
                 const Options& options,
                 bool navigationActive) {
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
    bool interactionPickReady = false;
    if (viewportHovered && options.depthGeometryReady && frame && options.exactView.valid
        && options.exactView.exactNative) {
        ViewportState& retained = state();
        if (retained.pendingPick.sequence != 0) {
            const inspection::PickResult completed = inspection::pick_result();
            if (completed.requestSequence == retained.pendingPick.sequence) {
                retained.gpuPickValid =
                    inspection::pick_result_fresh(completed,
                                                  retained.pendingPick,
                                                  frame.frameId,
                                                  options.exactView.engineFrame,
                                                  options.exactView.publication,
                                                  options.renderStatus.depthSequence,
                                                  graph.generation());
                if (retained.gpuPickValid) {
                    retained.gpuHovered = completed.node;
                    retained.gpuPickPointer = retained.pendingPickPointer;
                }
                retained.pendingPick = {};
            } else if (frame.frameId > retained.pendingPick.capturedFrame + 4U) {
                // A rejected/unavailable request must not permanently block newer
                // cursor samples.
                retained.pendingPick = {};
            }
        }
        constexpr float kPickRetentionRadius = 10.0F;
        const float retainedX = pointer.x - retained.gpuPickPointer.x;
        const float retainedY = pointer.y - retained.gpuPickPointer.y;
        if (retained.gpuPickValid
            && retainedX * retainedX + retainedY * retainedY
                   <= scaled(kPickRetentionRadius) * scaled(kPickRetentionRadius)) {
            result.hovered = retained.gpuHovered;
            interactionPickReady = true;
        } else {
            retained.gpuPickValid = false;
        }
        const inspection::PickCoordinate coordinate = inspection::map_pick_coordinate_to_viewport(
            pointer.x,
            pointer.y,
            imageMinimum.x,
            imageMinimum.y,
            imageMaximum.x,
            imageMaximum.y,
            options.exactView.viewport,
            static_cast<std::uint32_t>((std::max)(frame.width, 0.0F)),
            static_cast<std::uint32_t>((std::max)(frame.height, 0.0F)));
        if (coordinate.valid && retained.pendingPick.sequence == 0) {
            if (++retained.pickSequence == 0) {
                ++retained.pickSequence;
            }
            const inspection::PickRequest request{retained.pickSequence,
                                                  frame.frameId,
                                                  options.exactView.engineFrame,
                                                  options.exactView.publication,
                                                  options.renderStatus.depthSequence,
                                                  graph.generation(),
                                                  coordinate.x,
                                                  coordinate.y};
            inspection::publish_pick_request(request);
            retained.pendingPick = request;
            retained.pendingPickPointer = pointer;
        }
    }
    if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        result.navigation = camera.active;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || !camera.active
        || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        result.navigation = false;
    }

    const auto preparationStart = std::chrono::steady_clock::now();
    ViewportState& retained = state();
    std::vector<Projected>& projected = retained.projected;
    projected.clear();
    const bool sharedPlan =
        options.depthGeometryReady && options.presentation && options.exactView.valid
        && options.exactView.exactNative
        && options.exactView.engineFrame == options.renderStatus.engineFrame
        && options.exactView.publication == options.renderStatus.viewPublication
        && options.presentation->graphGeneration == graph.generation()
        && (options.renderStatus.batchSequence == 0
            || options.renderStatus.batchSequence == options.presentation->sequence);
    if (sharedPlan) {
        result.categoryFilteredNodes = options.presentation->stats.categoryFilteredNodes;
        result.hiddenNodes = options.presentation->stats.hiddenNodes;
        result.detailFilteredNodes = options.presentation->stats.detailFilteredNodes;
        projected.reserve(
            (std::max)(projected.capacity(), options.presentation->presentation.size()));
        for (const inspection::SceneFrame::PresentationEntry& entry :
             options.presentation->presentation) {
            if (entry.cullingReason != inspection::PresentationCullReason::none) {
                continue;
            }
            ++result.eligibleNodes;
            const inspection::Node* node = graph.node(entry.node);
            if (node == nullptr) {
                ++result.omittedNodes;
                ++result.staleNodes;
                continue;
            }
            ProjectedPoint point{};
            if (!project_exact(options.exactView, entry.center, imageMinimum, imageMaximum, point)
                || !inside(point.screen, imageMinimum, imageMaximum)) {
                ++result.omittedNodes;
                ++result.offscreenNodes;
                continue;
            }
            projected.push_back(
                {node->id, node->kind, point.screen, point.depth, helper_color(entry.color)});
            ++result.projectedNodes;
        }
    }
    result.preparationMilliseconds = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - preparationStart)
                                         .count();

    if (viewportHovered && interactionPickReady && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (!result.hovered) {
            result.clearSelection = true;
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            result.focused = result.hovered;
        } else {
            result.selected = result.hovered;
        }
    }
    if (viewportHovered && interactionPickReady && ImGui::IsMouseReleased(ImGuiMouseButton_Right)
        && result.hovered) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        const float contextThreshold = scaled(4.0F);
        if (drag.x * drag.x + drag.y * drag.y <= contextThreshold * contextThreshold) {
            result.context = result.hovered;
        }
    }

    // Prepare every label before touching the draw list.  LabelPlacement stores
    // ids and value copies only; no native Node* or projected scratch pointer is
    // retained across a graph refresh.
    std::vector<LabelPlacement>& labels = retained.labels;
    std::vector<label_layout::Candidate>& labelCandidates = retained.labelCandidates;
    labels.clear();
    labelCandidates.clear();
    labels.reserve((std::max)(labels.capacity(), projected.size()));
    labelCandidates.reserve((std::max)(labelCandidates.capacity(), projected.size()));
    for (const Projected& marker : projected) {
        const bool selectedNode = marker.id == selected;
        const bool hoveredNode = marker.id == result.hovered;
        if (options.depthGeometryReady && (selectedNode || hoveredNode || options.showLabels)) {
            const inspection::Node* node = graph.node(marker.id);
            if (node != nullptr) {
                const ImU32 color =
                    marker.plannedColor != 0 ? marker.plannedColor : helper_color(marker.kind);
                labels.push_back(make_label(*node,
                                            marker,
                                            color,
                                            imageMinimum,
                                            imageMaximum,
                                            selectedNode ? 2 : (hoveredNode ? 1 : 0)));
            }
        }
    }
    std::ranges::stable_sort(labels, [](const LabelPlacement& left, const LabelPlacement& right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        if (left.projected.depth != right.projected.depth) {
            return left.projected.depth < right.projected.depth;
        }
        return left.projected.id.value < right.projected.id.value;
    });
    for (const LabelPlacement& label : labels) {
        labelCandidates.push_back(label_layout::Candidate{label.minimum.x,
                                                          label.desiredY,
                                                          label.maximum.x - label.minimum.x,
                                                          label.maximum.y - label.minimum.y,
                                                          label.projected.depth,
                                                          label.projected.id.value,
                                                          label.priority});
    }
    const auto labelStart = std::chrono::steady_clock::now();
    const label_layout::Result labelResult = label_layout::place(
        std::span<const label_layout::Candidate>(labelCandidates.data(), labelCandidates.size()),
        label_layout::Rect{imageMinimum.x, imageMinimum.y, imageMaximum.x, imageMaximum.y},
        scaled(2.0F),
        scaled(32.0F),
        retained.labelResults);
    result.labelLayoutMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - labelStart)
            .count();
    result.attemptedLabels = labelResult.attempted;
    result.placedLabels = labelResult.placed;
    result.collisionOmittedLabels = labelResult.collisionOmitted;
    result.omittedLabels = labelResult.collisionOmitted;

    const bool waiting = !camera.projectionAvailable;
    const DrawPlan drawPlan =
        make_draw_plan(graph, labels, retained.labelResults, static_cast<bool>(frame), waiting);
    result.plannedVertices = drawPlan.vertices;
    result.plannedIndices = drawPlan.indices;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (!reserve_draw_list(*drawList, drawPlan)) {
        result.allocationFailure = true;
        result.reservationFailure = true;
        // Leave the list untouched: no image, helper, marker, or label has been
        // submitted, and retained projection/label caches remain available for
        // the next frame's retry.
        return skipped_result(result, true);
    }
    if (frame) {
        // Safety: clamp the image rect to the available area to prevent cut-off.
        ImVec2 safeMin = imageMinimum;
        ImVec2 safeMax = imageMaximum;
        safeMin.x = (std::max)(safeMin.x, areaMinimum.x);
        safeMin.y = (std::max)(safeMin.y, areaMinimum.y);
        safeMax.x = (std::min)(safeMax.x, areaMaximum.x);
        safeMax.y = (std::min)(safeMax.y, areaMaximum.y);
        drawList->PushClipRect(safeMin, safeMax, true);
        drawList->AddImage(frame.texture, safeMin, safeMax);
        drawList->PopClipRect();
    }
    drawList->PushClipRect(imageMinimum, imageMaximum, true);
    if (!camera.projectionAvailable) {
        const char* message = "Waiting for a valid native player-camera pose";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        const float imageWidth = imageMaximum.x - imageMinimum.x;
        const ImVec2 textPosition{imageMinimum.x + (imageWidth - textSize.x) * 0.5F,
                                  imageMinimum.y + scaled(12.0F)};
        drawList->AddRectFilled({textPosition.x - scaled(8.0F), textPosition.y - scaled(5.0F)},
                                {textPosition.x + textSize.x + scaled(8.0F),
                                 textPosition.y + textSize.y + scaled(5.0F)},
                                kLabelBackground,
                                scaled(2.0F));
        drawList->AddText(textPosition, kHiddenColor, message);
    } else {
        std::array<char, 160> disclosure{};
        std::snprintf(disclosure.data(),
                      disclosure.size(),
                      "%s · %s · %s",
                      camera.poseSource == client::viewer::camera::PoseSource::player
                          ? "PLAYER VIEW"
                          : "CAMERA ACTIVE",
                      inspection::overlay_detail_name(options.detail),
                      inspection::helper_backend_name(options.renderStatus.backend));
        const float textWidth = (std::max)(0.0F, imageMaximum.x - imageMinimum.x - scaled(16.0F));
        const auto measure = [](std::string_view value) noexcept {
            return ImGui::CalcTextSize(value.data(), value.data() + value.size()).x;
        };
        const std::string fittedDisclosure = fit_text(disclosure.data(), textWidth, measure);
        if (!fittedDisclosure.empty()) {
            const ImVec2 textSize = ImGui::CalcTextSize(fittedDisclosure.c_str());
            const ImVec2 textPos{imageMinimum.x + scaled(8.0F), imageMinimum.y + scaled(8.0F)};
            drawList->AddRectFilled(
                {textPos.x - scaled(5.0F), textPos.y - scaled(3.0F)},
                {textPos.x + textSize.x + scaled(5.0F), textPos.y + textSize.y + scaled(3.0F)},
                kLabelBackground,
                scaled(2.0F));
            drawList->AddText(textPos, kDisclosureColor, fittedDisclosure.c_str());
        }
        if (!options.depthGeometryReady) {
            const char* failure =
                inspection::helper_failure_name(options.renderStatus.failureReason);
            std::array<char, 160> message{};
            std::snprintf(message.data(), message.size(), "Depth helpers unavailable: %s", failure);
            const ImVec2 messageSize = ImGui::CalcTextSize(message.data());
            const ImVec2 messagePosition{
                imageMinimum.x + ((imageMaximum.x - imageMinimum.x) - messageSize.x) * 0.5F,
                imageMinimum.y + scaled(36.0F)};
            drawList->AddRectFilled(
                {messagePosition.x - scaled(8.0F), messagePosition.y - scaled(5.0F)},
                {messagePosition.x + messageSize.x + scaled(8.0F),
                 messagePosition.y + messageSize.y + scaled(5.0F)},
                kLabelBackground,
                scaled(2.0F));
            drawList->AddText(messagePosition, kHoverColor, message.data());
        }
    }
    for (const label_layout::Placement& placement : retained.labelResults) {
        if (placement.candidate >= labels.size()) {
            continue;
        }
        LabelPlacement label = labels[placement.candidate];
        label.minimum = {placement.rect.minimumX, placement.rect.minimumY};
        label.maximum = {placement.rect.maximumX, placement.rect.maximumY};
        draw_label(*drawList, label, graph);
    }
    if (result.categoryFilteredNodes != 0 || result.hiddenNodes != 0
        || result.detailFilteredNodes != 0 || result.omittedNodes != 0
        || result.omittedLabels != 0) {
        // One-line footer at the bottom of the image disclosing what this frame
        // silently dropped; silent when nothing was dropped.
        std::array<char, 256> footer{};
        int written = 0;
        const auto append = [&footer, &written](const char* what, std::size_t count) {
            if (count == 0 || written < 0 || static_cast<std::size_t>(written) >= footer.size()) {
                return;
            }
            written += std::snprintf(footer.data() + written,
                                     footer.size() - static_cast<std::size_t>(written),
                                     "%s%zu %s",
                                     written == 0 ? "" : " · ",
                                     count,
                                     what);
        };
        append("helpers category-disabled", result.categoryFilteredNodes);
        append("helpers manually hidden", result.hiddenNodes);
        append("helpers hidden by detail level", result.detailFilteredNodes);
        append("helpers off-image", result.omittedNodes);
        append("labels dropped", result.omittedLabels);
        if (written > 0) {
            const float textWidth =
                (std::max)(0.0F, imageMaximum.x - imageMinimum.x - scaled(16.0F));
            const auto measure = [](std::string_view value) noexcept {
                return ImGui::CalcTextSize(value.data(), value.data() + value.size()).x;
            };
            const std::string fittedFooter = fit_text(footer.data(), textWidth, measure);
            if (!fittedFooter.empty()) {
                const ImVec2 textSize = ImGui::CalcTextSize(fittedFooter.c_str());
                const ImVec2 position{imageMinimum.x + scaled(8.0F),
                                      imageMaximum.y - textSize.y - scaled(8.0F)};
                drawList->AddRectFilled({position.x - scaled(5.0F), position.y - scaled(3.0F)},
                                        {position.x + textSize.x + scaled(5.0F),
                                         position.y + textSize.y + scaled(3.0F)},
                                        kLabelBackground,
                                        scaled(2.0F));
                drawList->AddText(position, kDisclosureColor, fittedFooter.c_str());
            }
        }
    }
    drawList->PopClipRect();

    if (result.hovered) {
        const inspection::Node* node = graph.node(result.hovered);
        if (node != nullptr && (node->transform.has_value() || node->bounds.has_value())) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(node->name.c_str());
            ImGui::TextDisabled("%s", inspection::kind_name(node->kind));
            ImGui::Separator();
            if (node->transform.has_value()) {
                const auto& position = node->transform->position;
                ImGui::Text("Position  %.3f  %.3f  %.3f",
                            static_cast<double>(position[0]),
                            static_cast<double>(position[1]),
                            static_cast<double>(position[2]));
            }
            if (node->bounds.has_value() && inspection::bounds_valid(*node->bounds)) {
                draw_bounds_tooltip(*node->bounds);
                ImGui::TextDisabled("Bounds provenance: %s",
                                    node->boundsProvenance.has_value()
                                        ? inspection::provenance_name(*node->boundsProvenance)
                                        : "unknown");
            } else if (node->kind == inspection::NodeKind::trigger) {
                ImGui::TextDisabled("Shape unavailable; center observation only");
            } else {
                ImGui::TextDisabled("Bounds unavailable");
            }
            ImGui::EndTooltip();
        }
    }

    retained.lastComplete = result;
    retained.hasLastComplete = true;
    return result;
}

Result draw(const inspection::Graph& graph,
            inspection::NodeId selected,
            const client::viewer::camera::Status& camera,
            const client::hooks::graphics::renderer::frame_capture::View& frame,
            const Options& options,
            bool navigationActive) noexcept {
    try {
        return draw_impl(graph, selected, camera, frame, options, navigationActive);
    } catch (const std::bad_alloc&) {
        Result result{};
        result.navigation = navigationActive;
        return skipped_result(result, false);
    } catch (const std::length_error&) {
        Result result{};
        result.navigation = navigationActive;
        return skipped_result(result, false);
    }
}

void reset() noexcept {
    state() = ViewportState{};
}

} // namespace sunrise::client::ui::world_inspector::viewport
