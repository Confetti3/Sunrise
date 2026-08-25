#include "world_inspector_modules.h"

#include <array>

namespace sunrise::client::ui::world_inspector {
namespace {

constexpr std::array kViews{
    WorkspaceViewDescriptor{
        CenterMode::world, "World", "Copied spatial observations and explicitly known evidence."},
    WorkspaceViewDescriptor{
        CenterMode::nodeGraph, "Node Graph", "Ownership and filtered hierarchy navigation."},
    WorkspaceViewDescriptor{CenterMode::logicGraph,
                            "Relationships",
                            "Authored Activity Logic relations; unavailable without catalog "
                            "edges."},
    WorkspaceViewDescriptor{CenterMode::activityMap,
                            "Activity Map",
                            "Authored catalog layout; never treated as live transforms."},
};

constexpr std::array kPanels{
    BottomPanelDescriptor{BottomTab::references, "References"},
    BottomPanelDescriptor{BottomTab::data, "Data"},
    BottomPanelDescriptor{BottomTab::compare, "Compare"},
    BottomPanelDescriptor{BottomTab::diagnostics, "Diagnostics"},
};

} // namespace

std::span<const WorkspaceViewDescriptor> workspace_views() noexcept {
    return kViews;
}

std::span<const BottomPanelDescriptor> bottom_panels() noexcept {
    return kPanels;
}

} // namespace sunrise::client::ui::world_inspector
