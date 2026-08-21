#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include <imgui.h>

#include "../../hooks/graphics/renderer/graphics_frame_capture.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::viewport {

enum class Detail : std::uint8_t {
    selectedOnly,
    selectedNearby,
    all,
};

struct Options final {
    bool showGeometry{true};
    bool showEntities{true};
    bool showSpawns{true};
    bool showLogic{true};
    bool showTriggers{true};
    bool showAudio{true};
    bool showRendering{true};
    bool showNavigation{true};
    bool showLabels{};
    bool showKnownBounds{true};
    bool showTriggerCenters{true};
    bool showAuthoredOrientation{true};
    Detail detail{Detail::selectedNearby};
};

struct Result final {
    inspection::NodeId hovered{};
    inspection::NodeId selected{};
    inspection::NodeId focused{};
    inspection::NodeId context{};
    std::size_t detailFilteredNodes{};
    std::size_t omittedNodes{};
    std::size_t omittedSegments{};
    std::size_t omittedLabels{};
    bool clearSelection{};
    bool navigation{};
};

Result draw(const inspection::Graph& graph,
            inspection::NodeId selected,
            const std::unordered_set<std::uint64_t>& hidden,
            const std::unordered_set<std::uint64_t>& admitted,
            const client::viewer::camera::Status& camera,
            const client::hooks::graphics::renderer::frame_capture::View& frame,
            const Options& options,
            bool navigationActive) noexcept;

} // namespace sunrise::client::ui::world_inspector::viewport
