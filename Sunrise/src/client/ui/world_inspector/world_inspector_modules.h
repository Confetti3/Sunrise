#pragma once

#include <cstdint>
#include <span>

namespace sunrise::client::ui::world_inspector {

enum class CenterMode : std::uint8_t {
    world,
    nodeGraph,
    logicGraph,
    activityMap,
};

enum class BottomTab : std::uint8_t {
    references,
    data,
    events,
    compare,
    diagnostics,
};

struct WorkspaceViewDescriptor final {
    CenterMode mode{CenterMode::world};
    const char* label{};
    const char* summary{};
};

struct BottomPanelDescriptor final {
    BottomTab tab{BottomTab::references};
    const char* label{};
};

[[nodiscard]] std::span<const WorkspaceViewDescriptor> workspace_views() noexcept;
[[nodiscard]] std::span<const BottomPanelDescriptor> bottom_panels() noexcept;

} // namespace sunrise::client::ui::world_inspector
