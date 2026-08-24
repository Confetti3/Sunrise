#pragma once

#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../hooks/graphics/renderer/graphics_frame_capture.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../inspection/world_inspection_model.h"
#include "world_inspector_detail.h"

namespace sunrise::client::ui::world_inspector::viewport {

struct Options final {
    bool showGeometry{true};
    bool showEntities{true};
    bool showSpawns{true};
    bool showLogic{true};
    bool showTriggers{true};
    bool showAudio{true};
    bool showLabels{};
    bool showKnownBounds{true};
    bool showTriggerCenters{true};
    bool showAuthoredOrientation{true};
    Detail detail{Detail::adaptive};
    std::uint32_t maximumVisibleNodes{320};
    float nearbyRadius{100.0F};
    float glyphSizePixels{19.0F};
    float lineWidthPixels{2.4F};
    float baseOpacity{0.75F};
    float focusContextOpacity{0.50F};
    /** Exact captured frame contains native/depth geometry and can receive annotations. */
    bool depthGeometryReady{};
    inspection::RenderViewSnapshot exactView{};
    inspection::HelperRenderStatus renderStatus{};
    inspection::SceneFramePtr presentation{};
};

struct Result final {
    inspection::NodeId hovered{};
    inspection::NodeId selected{};
    inspection::NodeId focused{};
    inspection::NodeId context{};
    std::size_t categoryFilteredNodes{};
    std::size_t hiddenNodes{};
    std::size_t detailFilteredNodes{};
    std::size_t omittedNodes{};
    std::size_t omittedLabels{};
    std::size_t eligibleNodes{};
    std::size_t projectedNodes{};
    std::size_t offscreenNodes{};
    std::size_t staleNodes{};
    std::size_t attemptedLabels{};
    std::size_t placedLabels{};
    std::size_t collisionOmittedLabels{};
    std::size_t plannedVertices{};
    std::size_t plannedIndices{};
    double preparationMilliseconds{};
    double labelLayoutMilliseconds{};
    bool allocationFailure{};
    bool reservationFailure{};
    bool clearSelection{};
    bool navigation{};
};

[[nodiscard]] inspection::OverlayPolicy overlay_policy(const Options& options) noexcept;

/** Clears retained projection, label, and candidate-cache scratch state. */
void reset() noexcept;

Result draw(const inspection::Graph& graph,
            inspection::NodeId selected,
            const client::viewer::camera::Status& camera,
            const client::hooks::graphics::renderer::frame_capture::View& frame,
            const Options& options,
            bool navigationActive) noexcept;

} // namespace sunrise::client::ui::world_inspector::viewport
