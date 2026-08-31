#include "world_inspector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../../core/ui/memory/allocator.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../core/ui/textures/ui_texture_slots.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scenarios/definition.h"
#include "../../../state/build_data/worlds/world_catalog.h"
#include "../../content/statics/statics_footprints.h"
#include "../../content/statics/statics_probe.h"
#include "../../hooks/graphics/renderer/graphics_depth_observer.h"
#include "../../hooks/graphics/renderer/native_debug_renderer.h"
#include "../../hooks/graphics/renderer/renderer.h"
#include "../../hooks/retail_log/retail_log_enqueue_observer.h"
#include "../../hooks/squad_reference_probe/squad_reference_probe.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../inspection/inspection_capture.h"
#include "../../inspection/current_location_catalog.h"
#include "../../inspection/inspection_descriptors.h"
#include "../../inspection/inspection_session.h"
#include "../../inspection/inspection_settings_store.h"
#include "../../inspection/providers/spawn_inspection_provider.h"
#include "../../inspection/world_inspection_model.h"
#include "../../player/player_settings_store.h"
#include "../../viewer/viewer_input_ownership.h"
#include "../../../server/gameplay/physics/host/physics_session.h"
#include "../../../server/bap/encrypted/push/activity/activity_roster_research.h"
#include "world_debug_scene_lines.h"
#include "world_inspector_commands.h"
#include "world_inspector_graph.h"
#include "world_inspector_graph_admission.h"
#include "world_inspector_viewport.h"
#include "world_inspector_workspace_state.h"

namespace sunrise::client::ui::world_inspector {
namespace {

namespace camera = client::viewer::camera;
namespace dpi = core::ui::scaling::dpi;
namespace ui_memory = core::ui::memory;
namespace model = client::inspection;
namespace capture = client::inspection::capture;
namespace provider = client::inspection::providers;
namespace activity_catalog = client::inspection::activity_catalog;
namespace location_catalog = client::inspection::current_location_catalog;
namespace renderer = client::hooks::graphics::renderer;
namespace section = core::ui::components::section;
namespace textures = core::ui::textures;
namespace toggle = core::ui::components::toggle;
namespace viewer_input = client::viewer::input;
namespace scenario_state = state::build_data::scenarios;
namespace worlds = state::build_data::worlds;
namespace roster_push = server::bap::encrypted::push::activity;

using commands::copy_camera_position;
using commands::copy_id;
using commands::copy_position;
using commands::copy_tag;
using commands::copy_text;

constexpr float kToolbarHeight = 30.0F;
constexpr float kStatusHeight = 30.0F;
constexpr float kSplitterThickness = 5.0F;
constexpr float kMinimumViewportWidth = 360.0F;
constexpr float kMinimumMainHeight = 140.0F;
constexpr float kMinimumBottomUsableHeight = 40.0F;
constexpr float kMinimumTreeRowHeight = 28.0F;
constexpr float kInspectorRowPadding = 6.0F;
constexpr float kTreeIndent = 14.0F;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::uint64_t kTrostlandEventVolume = 0xA55DF98A17C13E3CULL;
constexpr ImU32 kGuideColor = IM_COL32(63, 55, 42, 210);
constexpr ImVec4 kSelection{0.796F, 0.608F, 0.318F, 1.0F};

struct WorkspaceViewDescriptor final {
    CenterMode mode;
    const char* label;
    const char* summary;
};

struct BottomPanelDescriptor final {
    BottomTab tab;
    const char* label;
};

constexpr std::array kWorkspaceViews{
    WorkspaceViewDescriptor{
        CenterMode::world, "World", "Copied spatial observations and explicitly known evidence."},
    WorkspaceViewDescriptor{CenterMode::overview,
                            "Overview",
                            "Current-activity ownership, authored links, and relationships."},
    WorkspaceViewDescriptor{
        CenterMode::nodeGraph, "Node Graph", "Ownership and filtered hierarchy navigation."},
    WorkspaceViewDescriptor{CenterMode::relationships,
                            "Relationships",
                            "Authored Activity Logic relations; unavailable without catalog edges."},
    WorkspaceViewDescriptor{CenterMode::activityMap,
                            "Activity Map",
                            "Authored catalog layout; never treated as live transforms."},
};

constexpr std::array kBottomPanels{
    BottomPanelDescriptor{BottomTab::references, "References"},
    BottomPanelDescriptor{BottomTab::data, "Data"},
    BottomPanelDescriptor{BottomTab::compare, "Compare"},
    BottomPanelDescriptor{BottomTab::diagnostics, "Diagnostics"},
};
constexpr ImVec4 kSpawn{0.965F, 0.886F, 0.478F, 1.0F};
constexpr ImVec4 kWarning{0.949F, 0.549F, 0.216F, 1.0F};
constexpr ImVec4 kFailure{0.941F, 0.349F, 0.349F, 1.0F};
constexpr ImVec4 kMuted{0.50F, 0.55F, 0.61F, 1.0F};

[[nodiscard]] float half_fov_radians(float fov) noexcept {
    return fov <= kPi + 0.1F ? fov * 0.5F : fov * (kPi / 360.0F);
}

namespace status_layout {

struct Regions final {
    float contentMinimum{};
    float leftMaximum{};
    float badgeMinimum{};
    float contentMaximum{};
};

[[nodiscard]] constexpr Regions
compute(float contentMinimum, float contentMaximum, float gap, float badgeWidth) noexcept {
    contentMaximum = (std::max)(contentMinimum, contentMaximum);
    gap = (std::max)(0.0F, gap);
    badgeWidth = std::clamp(badgeWidth, 0.0F, contentMaximum - contentMinimum);
    const float badgeMinimum = contentMaximum - badgeWidth;
    return {contentMinimum,
            (std::max)(contentMinimum, badgeMinimum - gap),
            badgeMinimum,
            contentMaximum};
}

[[nodiscard]] constexpr bool
item_fits(float previousMaximum, float spacing, float itemWidth, float leftMaximum) noexcept {
    return previousMaximum + (std::max)(0.0F, spacing) + (std::max)(0.0F, itemWidth) <= leftMaximum;
}

} // namespace status_layout

constexpr ImGuiWindowFlags kWorkspaceFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

[[nodiscard]] constexpr float
padded_row_height(float textHeight, float verticalPadding, float minimumHeight) noexcept {
    return (std::max)(minimumHeight, textHeight + verticalPadding * 2.0F);
}

[[nodiscard]] constexpr bool
fully_visible(float minimum, float maximum, float clipMinimum, float clipMaximum) noexcept {
    return minimum >= clipMinimum && maximum <= clipMaximum;
}

[[nodiscard]] constexpr float snap_scroll(float value, float rowHeight, float maximum) noexcept {
    const float clamped = std::clamp(value, 0.0F, (std::max)(0.0F, maximum));
    if (rowHeight <= 0.0F) {
        return clamped;
    }
    const auto row = static_cast<std::uint64_t>(clamped / rowHeight + 0.5F);
    return (std::min)(static_cast<float>(row) * rowHeight, maximum);
}

[[nodiscard]] constexpr float
aligned_scroll_padding(float contentHeight, float visibleHeight, float rowHeight) noexcept {
    if (rowHeight <= 0.0F || visibleHeight <= 0.0F) {
        return 0.0F;
    }
    const float rawMaximum = (std::max)(0.0F, contentHeight - visibleHeight);
    auto rows = static_cast<std::uint64_t>(rawMaximum / rowHeight);
    if (static_cast<float>(rows) * rowHeight < rawMaximum) {
        ++rows;
    }
    const float alignedMaximum = static_cast<float>(rows) * rowHeight;
    return (std::max)(0.0F, alignedMaximum + visibleHeight - contentHeight);
}

static_assert(padded_row_height(16.0F, 6.0F, 28.0F) == 28.0F);
static_assert(padded_row_height(20.0F, 6.0F, 28.0F) == 32.0F);
static_assert(fully_visible(10.0F, 20.0F, 10.0F, 20.0F));
static_assert(!fully_visible(9.0F, 20.0F, 10.0F, 20.0F));
static_assert(!fully_visible(10.0F, 21.0F, 10.0F, 20.0F));
static_assert(snap_scroll(13.0F, 28.0F, 196.0F) == 0.0F);
static_assert(snap_scroll(15.0F, 28.0F, 196.0F) == 28.0F);
static_assert(snap_scroll(250.0F, 28.0F, 196.0F) == 196.0F);
static_assert(aligned_scroll_padding(280.0F, 100.0F, 28.0F) == 16.0F);

struct VerticalLayout final {
    float mainHeight{};
    float splitterY{};
    float bottomY{};
    float bottomHeight{};
    float statusY{};
    float minimumBottom{};
    float maximumBottom{};
    bool bottomVisible{};
};

[[nodiscard]] constexpr VerticalLayout compute_vertical_layout(float availableHeight,
                                                               float statusHeight,
                                                               float splitterSize,
                                                               float minimumMainHeight,
                                                               float requestedBottomHeight,
                                                               float minimumUsableBottomHeight,
                                                               float preferredMinimumBottomHeight,
                                                               bool collapsed) noexcept {
    availableHeight = (std::max)(0.0F, availableHeight);
    statusHeight = std::clamp(statusHeight, 0.0F, availableHeight);
    splitterSize = (std::max)(0.0F, splitterSize);
    minimumMainHeight = (std::max)(0.0F, minimumMainHeight);
    minimumUsableBottomHeight = (std::max)(0.0F, minimumUsableBottomHeight);
    preferredMinimumBottomHeight = (std::max)(0.0F, preferredMinimumBottomHeight);

    const float statusY = availableHeight - statusHeight;
    const float maximumBottom = (std::max)(0.0F, statusY - minimumMainHeight - splitterSize);
    const bool visible = !collapsed && maximumBottom >= minimumUsableBottomHeight;
    if (!visible) {
        return VerticalLayout{
            (std::max)(1.0F, statusY), statusY, statusY, 0.0F, statusY, 0.0F, 0.0F, false};
    }

    const float minimumBottom = (std::min)(preferredMinimumBottomHeight, maximumBottom);
    const float bottomHeight = std::clamp(requestedBottomHeight, minimumBottom, maximumBottom);
    const float bottomY = statusY - bottomHeight;
    const float splitterY = bottomY - splitterSize;
    return VerticalLayout{(std::max)(1.0F, splitterY),
                          splitterY,
                          bottomY,
                          bottomHeight,
                          statusY,
                          minimumBottom,
                          maximumBottom,
                          true};
}

constexpr VerticalLayout kNormalLayout =
    compute_vertical_layout(600.0F, 30.0F, 5.0F, 140.0F, 180.0F, 40.0F, 120.0F, false);
static_assert(kNormalLayout.bottomVisible);
static_assert(kNormalLayout.statusY == 570.0F);
static_assert(kNormalLayout.bottomHeight == 180.0F);
static_assert(kNormalLayout.bottomY == 390.0F);
static_assert(kNormalLayout.splitterY == 385.0F);
static_assert(kNormalLayout.mainHeight == 385.0F);

constexpr VerticalLayout kCollapsedLayout =
    compute_vertical_layout(600.0F, 30.0F, 5.0F, 140.0F, 180.0F, 40.0F, 120.0F, true);
static_assert(!kCollapsedLayout.bottomVisible);
static_assert(kCollapsedLayout.mainHeight == 570.0F);

constexpr VerticalLayout kTightLayout =
    compute_vertical_layout(180.0F, 30.0F, 5.0F, 140.0F, 120.0F, 40.0F, 120.0F, false);
static_assert(!kTightLayout.bottomVisible);
static_assert(kTightLayout.mainHeight == 150.0F);

using NodeIdentity = model::NodeKey;

struct SplitterResult final {
    bool changed{};
    bool released{};
};

std::atomic_bool g_open{};
WorkspaceState g_state{};

[[nodiscard]] model::OverlayPolicy workspace_overlay_policy() noexcept {
    return {g_state.showGeometry,
            g_state.showEntities,
            g_state.showSpawns,
            g_state.showLogic,
            g_state.showTriggers,
            g_state.showAudio,
            g_state.showLabels,
            g_state.showKnownBounds,
            g_state.showTriggerCenters,
            g_state.showAuthoredOrientation,
            g_state.overlayDetail,
            g_state.maximumVisibleNodes,
            g_state.nearbyRadius,
            g_state.glyphSizePixels,
            g_state.lineWidthPixels,
            g_state.baseOpacity,
            g_state.focusContextOpacity};
}

[[nodiscard]] viewport::Options viewport_options(const model::OverlayPolicy& policy) noexcept {
    return {policy.showGeometry,
            policy.showEntities,
            policy.showSpawns,
            policy.showLogic,
            policy.showTriggers,
            policy.showAudio,
            policy.showLabels,
            policy.showKnownBounds,
            policy.showTriggerCenters,
            policy.showAuthoredOrientation,
            policy.detail,
            policy.maximumVisibleNodes,
            policy.nearbyRadius,
            policy.glyphSizePixels,
            policy.lineWidthPixels,
            policy.baseOpacity,
            policy.focusContextOpacity};
}

[[nodiscard]] float scaled(float value) noexcept {
    return dpi::pixels(value);
}

[[nodiscard]] float control_height() noexcept {
    return ImGui::GetFrameHeight();
}

[[nodiscard]] const char* camera_phase_label(camera::Phase phase) noexcept {
    switch (phase) {
    case camera::Phase::inactive:
        return "Player view";
    case camera::Phase::entering:
        return "Entering";
    case camera::Phase::active:
        return "Active";
    case camera::Phase::exiting:
        return "Exiting";
    case camera::Phase::faulted:
        return "Faulted";
    }
    return "Unknown";
}

[[nodiscard]] const char* camera_source_label(camera::PoseSource source) noexcept {
    switch (source) {
    case camera::PoseSource::unavailable:
        return "No pose";
    case camera::PoseSource::player:
        return "Player camera";
    case camera::PoseSource::viewer:
        return "Detached";
    }
    return "Unknown";
}

[[nodiscard]] bool fully_visible(const ImVec2& minimum, const ImVec2& maximum) noexcept {
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 clipMinimum = drawList->GetClipRectMin();
    const ImVec2 clipMaximum = drawList->GetClipRectMax();
    return fully_visible(minimum.y, maximum.y, clipMinimum.y, clipMaximum.y);
}

[[nodiscard]] bool next_item_fully_visible(float height) noexcept {
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    return fully_visible(minimum, {minimum.x, minimum.y + height});
}

[[nodiscard]] const model::InspectionDocument& world() noexcept {
    return g_state.session.document();
}

[[nodiscard]] const model::WorldContext& world_context() noexcept {
    return world().context;
}

[[nodiscard]] bool current_catalog_scope(location_catalog::Scope& output) noexcept {
    output = {};
    const model::WorldContext& context = world_context();
    if (!context.sessionPresent || context.activitySession == 0 || context.scenarioTag == 0
        || context.packageName.empty()) {
        return false;
    }
    scenario_state::Definition layout{};
    if (!state::build_data::find_scenario_layout(context.packageName, layout)
        || layout.packageCount == 0 || layout.packageCount > layout.packages.size()) {
        return false;
    }
    output.activitySession = context.activitySession;
    output.scenarioTag = context.scenarioTag;
    output.packageName = context.packageName;
    output.mapFamily = context.mapStem.empty() ? context.packageName : context.mapStem;
    output.packageCount = layout.packageCount;
    std::copy(layout.packages.begin(),
              layout.packages.begin() + layout.packageCount,
              output.packageIds.begin());
    return true;
}

[[nodiscard]] std::string normalized_map_family(std::string_view family) {
    constexpr std::string_view destination = "_destination";
    constexpr std::string_view freeroam = "_freeroam";
    if (family.ends_with(destination)) {
        family.remove_suffix(destination.size());
    } else if (family.ends_with(freeroam)) {
        family.remove_suffix(freeroam.size());
    }
    return std::string(family);
}

[[nodiscard]] bool activity_selection(const worlds::Summary& summary,
                                      location_catalog::ActivitySelection& output) noexcept {
    output = {};
    const std::string_view name = worlds::name_of(summary);
    scenario_state::Definition layout{};
    if (name.empty() || world_context().activitySession == 0
        || !state::build_data::find_scenario_layout(name, layout) || layout.packageCount == 0
        || layout.packageCount > layout.packages.size()) {
        return false;
    }
    output.displayName.assign(name);
    output.scope.activitySession = world_context().activitySession;
    output.scope.scenarioTag = summary.scenarioTag;
    output.scope.packageName.assign(name);
    const std::string_view stem = worlds::stem_of(summary);
    output.scope.mapFamily.assign(stem.empty() ? name : stem);
    output.scope.packageCount = layout.packageCount;
    std::copy(layout.packages.begin(),
              layout.packages.begin() + layout.packageCount,
              output.scope.packageIds.begin());
    return true;
}

[[nodiscard]] bool contains_ascii_case_insensitive(std::string_view text,
                                                   std::string_view query) noexcept {
    if (query.empty()) {
        return true;
    }
    return std::search(text.begin(), text.end(), query.begin(), query.end(), [](char left, char right) {
               return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(left)))
                      == static_cast<unsigned char>(
                          std::tolower(static_cast<unsigned char>(right)));
           })
           != text.end();
}

[[nodiscard]] bool activity_matches(const worlds::Summary& summary,
                                    std::string_view query) noexcept {
    if (query.empty() || contains_ascii_case_insensitive(worlds::name_of(summary), query)
        || contains_ascii_case_insensitive(worlds::stem_of(summary), query)) {
        return true;
    }
    std::array<char, 16> tag{};
    std::snprintf(tag.data(), tag.size(), "0x%08X", summary.scenarioTag);
    return contains_ascii_case_insensitive(tag.data(), query);
}

template <typename Value> [[nodiscard]] const Value* root_property(std::string_view key) noexcept {
    const model::Node* root = world().graph.node(world().graph.root());
    if (root == nullptr) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(
        root->properties, [key](const model::Property& property) { return property.key == key; });
    return found == root->properties.end() ? nullptr : std::get_if<Value>(&found->value);
}

[[nodiscard]] std::uint64_t root_u64(std::string_view key) noexcept {
    const std::uint64_t* value = root_property<std::uint64_t>(key);
    return value == nullptr ? 0 : *value;
}

[[nodiscard]] std::uint32_t producer_epoch() noexcept {
    std::uint32_t result = 1;
    for (const model::ProviderReport& report : world().providerReports) {
        result = (std::max)(result, report.epoch);
    }
    return result;
}

void publish_scene_frame(const camera::Status& cameraStatus,
                         const renderer::frame_capture::View& capturedFrame) {
    static std::uint64_t sequence{};
    model::SceneFrame scene{};
    scene.sequence = ++sequence;
    scene.sourceFrame = capturedFrame.frameId;
    scene.graphGeneration = world().graph.generation();
    if (visible() && capturedFrame) {
        scene.lines.resize(debug_scene::kMaximumLines);
        const viewport::DetailAnchor anchor = viewport::detail_anchor(
            world().graph, g_state.selection.selected(), cameraStatus.pose.position);
        const model::RenderViewSnapshot exactView = renderer::native_debug::view();
        debug_scene::CollectQuery query{g_state.selection.selected(),
                                        cameraStatus.pose.position,
                                        anchor.center,
                                        workspace_overlay_policy(),
                                        debug_scene::kMaximumBoxes,
                                        exactView,
                                        g_state.selection.hovered()};
        thread_local std::string liveMapFamily;
        liveMapFamily = normalized_map_family(
            world_context().mapStem.empty() ? std::string_view(world_context().packageName)
                                            : std::string_view(world_context().mapStem));
        query.liveMapFamily = liveMapFamily;
        query.previousPresentation = debug_scene::frame();
        scene.stats =
            debug_scene::collect(scene, world().graph, g_state.admitted, g_state.hidden, query);
        model::assign_pick_tokens(scene);
    }
    g_state.session.publish_overlay(std::move(scene));
}

struct InspectorCapabilities final {
    std::size_t liveNodes{};
    std::size_t spatialNodes{};
    std::size_t knownBounds{};
    std::size_t triggerCenters{};
    std::size_t triggerShapes{};
    std::size_t logicDefinitions{};
    std::size_t logicPlacements{};
    std::size_t logicVariables{};
    std::size_t warnings{};
    std::size_t errors{};
};

[[nodiscard]] InspectorCapabilities capabilities() noexcept {
    InspectorCapabilities result{};
    for (const model::Node& node : world().graph.nodes()) {
        if (node.producer == model::Producer::activityCatalog) {
            continue;
        }
        if (node.producer == model::Producer::activityLogicCatalog) {
            if (node.kind == model::NodeKind::logicEntity) {
                ++result.logicDefinitions;
            } else if (node.kind == model::NodeKind::logicPlacement) {
                ++result.logicPlacements;
            } else if (node.kind == model::NodeKind::logicVariable) {
                ++result.logicVariables;
            }
            continue;
        }
        ++result.liveNodes;
        if (model::has_spatial_data(node)) {
            ++result.spatialNodes;
        }
        const bool bounds = node.bounds.has_value() && model::bounds_valid(*node.bounds);
        if (bounds) {
            ++result.knownBounds;
        }
        if (node.kind == model::NodeKind::trigger && node.transform.has_value()) {
            ++result.triggerCenters;
            if (bounds) {
                ++result.triggerShapes;
            }
        }
    }
    for (const model::Diagnostic& diagnostic : world().diagnostics) {
        if (diagnostic.severity == model::Diagnostic::Severity::warning) {
            ++result.warnings;
        } else if (diagnostic.severity == model::Diagnostic::Severity::error) {
            ++result.errors;
        }
    }
    return result;
}

[[nodiscard]] const model::Node* selected_node() noexcept {
    return world().graph.node(g_state.selection.selected());
}

[[nodiscard]] NodeIdentity node_identity(const model::Node* node) {
    return node == nullptr ? NodeIdentity{} : node->key;
}

[[nodiscard]] bool identity_matches(const NodeIdentity& identity,
                                    const model::Node& node) noexcept {
    return static_cast<bool>(identity) && identity == node.key;
}

[[nodiscard]] bool hidden(model::NodeId id) noexcept {
    const model::NodeKey key = world().graph.key(id);
    return static_cast<bool>(key) && g_state.hiddenKeys.contains(key);
}

void note_visibility_change() noexcept {
    ++g_state.hiddenRevision;
    g_state.rowsValid = false;
}

void show_all() noexcept {
    if (!g_state.hiddenKeys.empty()) {
        g_state.hidden.clear();
        g_state.hiddenKeys.clear();
        note_visibility_change();
    }
}

void toggle_hidden(model::NodeId id) noexcept {
    const model::Node* node = world().graph.node(id);
    if (node == nullptr || !model::supports(node->actions, model::Action::hide)) {
        return;
    }
    if (g_state.hiddenKeys.contains(node->key)) {
        g_state.hiddenKeys.erase(node->key);
        g_state.hidden.erase(id.value);
    } else {
        g_state.hiddenKeys.insert(node->key);
        g_state.hidden.insert(id.value);
    }
    note_visibility_change();
}

void isolate(model::NodeId id) noexcept {
    const model::Node* selected = world().graph.node(id);
    if (selected == nullptr || !model::supports(selected->actions, model::Action::isolate)) {
        return;
    }
    g_state.hidden.clear();
    g_state.hiddenKeys.clear();
    for (const model::Node& node : world().graph.nodes()) {
        if (model::supports(node.actions, model::Action::hide) && node.id != id) {
            g_state.hidden.insert(node.id.value);
            g_state.hiddenKeys.insert(node.key);
        }
    }
    note_visibility_change();
}

struct CameraTeleportAvailability final {
    camera::Pose pose{};
    const char* unavailableReason{};

    [[nodiscard]] bool available() const noexcept {
        return unavailableReason == nullptr;
    }
};

[[nodiscard]] CameraTeleportAvailability
camera_teleport_availability(model::NodeId id, const camera::Status& status) noexcept {
    CameraTeleportAvailability result{};
    const model::Node* node = world().graph.node(id);
    if (node == nullptr) {
        result.unavailableReason = "The selection is stale for the current Inspector graph.";
        return result;
    }
    if (!status.active) {
        result.unavailableReason = "Viewer Camera must be active.";
        return result;
    }
    const bool validBounds = node->bounds.has_value() && model::bounds_valid(*node->bounds);
    if (!validBounds && !node->transform.has_value()) {
        result.unavailableReason = node->bounds.has_value()
                                       ? "The selection has invalid world-space bounds."
                                       : "The selection has no world-space position or bounds.";
        return result;
    }
    std::array<float, 3> target{};
    float distance = 6.0F;
    if (validBounds) {
        target = model::bounds_center(*node->bounds);
        const auto extents = model::bounds_extents(*node->bounds);
        const float radius = (std::max)({extents[0], extents[1], extents[2], 0.0F});
        const float halfFov = half_fov_radians(status.pose.fov);
        const float tangent = std::tan(halfFov);
        if (std::isfinite(radius) && std::isfinite(tangent) && tangent > 0.05F) {
            distance = (std::max)(distance, radius / tangent * 2.0F);
        }
    } else {
        target = node->transform->position;
    }
    result.pose = status.pose;
    for (std::size_t lane = 0; lane < result.pose.position.size(); ++lane) {
        result.pose.position[lane] = target[lane] - result.pose.forward[lane] * distance;
    }
    if (!std::ranges::all_of(result.pose.position,
                             [](float lane) { return std::isfinite(lane); })) {
        result.unavailableReason = "The camera destination is not finite.";
    }
    return result;
}

void focus(model::NodeId selection) noexcept {
    const CameraTeleportAvailability availability =
        camera_teleport_availability(selection, camera::status());
    if (!availability.available()) {
        return;
    }
    (void)camera::move_to(availability.pose);
}

void select_node(model::NodeId id) noexcept {
    if (const model::Node* node = world().graph.node(id); node != nullptr) {
        g_state.selection.select(id);
        g_state.selectedKey = node->key;
        if (node->activityMetadata.has_value() && node->activityMetadata->graphHash != 0) {
            g_state.selectedActivityGraphHash = node->activityMetadata->graphHash;
        }
        g_state.revealSelection = true;
    }
}

void open_activity_graph(std::uint32_t graphHash) noexcept {
    if (graphHash == 0) {
        return;
    }
    g_state.selectedActivityGraphHash = graphHash;
    g_state.centerMode = CenterMode::activityMap;
    for (const model::Node& candidate : world().graph.nodes()) {
        if (candidate.producer == model::Producer::activityCatalog
            && candidate.kind == model::NodeKind::activityGraph
            && candidate.activityMetadata.has_value()
            && candidate.activityMetadata->graphHash == graphHash) {
            select_node(candidate.id);
            break;
        }
    }
}

void select_and_focus(model::NodeId id) noexcept {
    if (world().graph.node(id) != nullptr) {
        select_node(id);
        focus(id);
    }
}

void persist_layout() noexcept {
    const float scale = (std::max)(dpi::current(), 0.01F);
    model::settings::Settings settings = model::settings::get();
    settings.leftWidth = std::clamp(g_state.leftWidth / scale,
                                    model::settings::kMinimumLeftWidth,
                                    model::settings::kMaximumLeftWidth);
    settings.rightWidth = std::clamp(g_state.rightWidth / scale,
                                     model::settings::kMinimumRightWidth,
                                     model::settings::kMaximumRightWidth);
    settings.bottomHeight = std::clamp(g_state.bottomHeight / scale,
                                       model::settings::kMinimumBottomHeight,
                                       model::settings::kMaximumBottomHeight);
    settings.bottomCollapsed = g_state.bottomCollapsed;
    settings.showGeometry = g_state.showGeometry;
    settings.showEntities = g_state.showEntities;
    settings.showSpawns = g_state.showSpawns;
    settings.showLogic = g_state.showLogic;
    settings.showTriggers = g_state.showTriggers;
    settings.showAudio = g_state.showAudio;
    settings.showKnownBounds = g_state.showKnownBounds;
    settings.showTriggerCenters = g_state.showTriggerCenters;
    settings.showAuthoredOrientation = g_state.showAuthoredOrientation;
    settings.showLabels = g_state.showLabels;
    settings.overlayDetail = static_cast<std::uint8_t>(g_state.overlayDetail);
    settings.maximumVisibleNodes = g_state.maximumVisibleNodes;
    settings.nearbyRadius = g_state.nearbyRadius;
    settings.glyphSizePixels = g_state.glyphSizePixels;
    settings.lineWidthPixels = g_state.lineWidthPixels;
    settings.baseOpacity = g_state.baseOpacity;
    settings.focusContextOpacity = g_state.focusContextOpacity;
    (void)model::settings::publish(settings);
    g_state.layoutDirty = false;
}

void reset_layout() noexcept {
    g_state.leftWidth = scaled(model::settings::kDefaultLeftWidth);
    g_state.rightWidth = scaled(model::settings::kDefaultRightWidth);
    g_state.bottomHeight = scaled(model::settings::kDefaultBottomHeight);
    g_state.bottomCollapsed = false;
    g_state.layoutScale = (std::max)(dpi::current(), 0.01F);
    g_state.layoutDirty = true;
    persist_layout();
}

void initialize_layout() noexcept {
    if (g_state.layoutInitialized) {
        return;
    }
    const model::settings::Settings settings = model::settings::get();
    g_state.leftWidth = scaled(settings.leftWidth);
    g_state.rightWidth = scaled(settings.rightWidth);
    g_state.bottomHeight = scaled(settings.bottomHeight);
    g_state.bottomCollapsed = settings.bottomCollapsed;
    g_state.showGeometry = settings.showGeometry;
    g_state.showEntities = settings.showEntities;
    g_state.showSpawns = settings.showSpawns;
    g_state.showLogic = settings.showLogic;
    g_state.showTriggers = settings.showTriggers;
    g_state.showAudio = settings.showAudio;
    g_state.showKnownBounds = settings.showKnownBounds;
    g_state.showTriggerCenters = settings.showTriggerCenters;
    g_state.showAuthoredOrientation = settings.showAuthoredOrientation;
    g_state.showLabels = settings.showLabels;
    g_state.overlayDetail = static_cast<viewport::Detail>(settings.overlayDetail);
    g_state.maximumVisibleNodes = settings.maximumVisibleNodes;
    g_state.nearbyRadius = settings.nearbyRadius;
    g_state.glyphSizePixels = settings.glyphSizePixels;
    g_state.lineWidthPixels = settings.lineWidthPixels;
    g_state.baseOpacity = settings.baseOpacity;
    g_state.focusContextOpacity = settings.focusContextOpacity;
    g_state.layoutScale = (std::max)(dpi::current(), 0.01F);
    g_state.layoutInitialized = true;
}

void refresh_layout_scale() noexcept {
    const float current = (std::max)(dpi::current(), 0.01F);
    if (!g_state.layoutInitialized || current == g_state.layoutScale) {
        return;
    }
    const float ratio = current / g_state.layoutScale;
    g_state.leftWidth *= ratio;
    g_state.rightWidth *= ratio;
    g_state.bottomHeight *= ratio;
    g_state.layoutScale = current;
}

[[nodiscard]] std::string root_label(HierarchyMode mode,
                                     const model::InspectionDocument& document) {
    const model::WorldContext& context = document.context;
    std::array<char, 128> text{};
    switch (mode) {
    case HierarchyMode::world:
        return context.packageName.empty() ? std::string("World") : context.packageName;
    case HierarchyMode::source: {
        const int written =
            context.scenarioTag == 0
                ? std::snprintf(text.data(), text.size(), "Source / unresolved")
                : std::snprintf(
                      text.data(), text.size(), "Source / scenario 0x%08X", context.scenarioTag);
        return written > 0 && static_cast<std::size_t>(written) < text.size()
                   ? std::string(text.data(), static_cast<std::size_t>(written))
                   : std::string("Source");
    }
    case HierarchyMode::activity: {
        const int written =
            context.activitySession == 0
                ? std::snprintf(text.data(), text.size(), "Activity / unresolved")
                : std::snprintf(text.data(),
                                text.size(),
                                "Activity / 0x%016llX",
                                static_cast<unsigned long long>(context.activitySession));
        return written > 0 && static_cast<std::size_t>(written) < text.size()
                   ? std::string(text.data(), static_cast<std::size_t>(written))
                   : std::string("Activity");
    }
    }
    return "World";
}

[[nodiscard]] FilterGroup filter_group(model::NodeKind kind) noexcept {
    switch (model::descriptor(kind).category) {
    case model::NodeCategory::geometry:
        return FilterGroup::geometry;
    case model::NodeCategory::entity:
    case model::NodeCategory::physics:
        return FilterGroup::entities;
    case model::NodeCategory::spawn:
        return FilterGroup::spawns;
    case model::NodeCategory::logic:
        return FilterGroup::logic;
    case model::NodeCategory::trigger:
        return FilterGroup::triggers;
    case model::NodeCategory::audio:
        return FilterGroup::audio;
    default:
        return FilterGroup::entities;
    }
}

[[nodiscard]] bool is_structural(model::NodeKind kind) noexcept {
    return kind == model::NodeKind::world || kind == model::NodeKind::source
           || kind == model::NodeKind::activity || kind == model::NodeKind::activityGraph
           || kind == model::NodeKind::activityGraphNode
           || kind == model::NodeKind::activityReference || kind == model::NodeKind::destination
           || kind == model::NodeKind::unresolved;
}

[[nodiscard]] bool filter_enabled(FilterGroup group) noexcept {
    switch (group) {
    case FilterGroup::geometry:
        return g_state.showGeometry;
    case FilterGroup::entities:
        return g_state.showEntities;
    case FilterGroup::spawns:
        return g_state.showSpawns;
    case FilterGroup::logic:
        return g_state.showLogic;
    case FilterGroup::triggers:
        return g_state.showTriggers;
    case FilterGroup::audio:
        return g_state.showAudio;
    }
    return true;
}

[[nodiscard]] bool filter_present(FilterGroup group) noexcept {
    return std::ranges::any_of(world().graph.nodes(), [group](const model::Node& node) {
        return node.producer != model::Producer::activityCatalog && !is_structural(node.kind)
               && filter_group(node.kind) == group;
    });
}

[[nodiscard]] bool node_admitted_by_filters(const model::Node& node,
                                            const model::Query& query) noexcept {
    // Activity graphs are package-authored metadata. Keeping them out of live admission prevents
    // hundreds of non-spatial rows from overwhelming the Scene Tree and ownership graph.
    if (node.producer == model::Producer::activityCatalog) {
        return false;
    }
    if (!is_structural(node.kind) && !filter_enabled(filter_group(node.kind))) {
        return false;
    }
    if (!g_state.showHidden && hidden(node.id)) {
        return false;
    }
    if (g_state.errorsOnly && node.status != model::Status::failed) {
        return false;
    }
    return model::matches(node, query);
}

[[nodiscard]] bool overview_node_admitted_by_filters(const model::Node& node,
                                                     const model::Query& query) noexcept {
    if (!is_structural(node.kind) && !filter_enabled(filter_group(node.kind))) {
        return false;
    }
    if (!g_state.showHidden && hidden(node.id)) {
        return false;
    }
    if (g_state.errorsOnly && node.status != model::Status::failed) {
        return false;
    }
    return model::matches(node, query);
}

[[nodiscard]] overview::ActivityScope current_activity_scope() {
    const location_catalog::PreviewStatus preview = location_catalog::preview_status();
    overview::ActivityScope scope{};
    if (preview.active) {
        scope.label = preview.displayName.empty() ? "Authored activity" : preview.displayName;
        scope.scenarioTag = preview.scenarioTag;
        scope.available = preview.scenarioTag != 0;
        scope.preview = true;
        return scope;
    }
    const model::WorldContext& context = world_context();
    scope.label = context.packageName.empty() ? "Current activity" : context.packageName;
    scope.activitySession = context.activitySession;
    scope.scenarioTag = context.scenarioTag;
    scope.available = context.sessionPresent && context.activitySession != 0
                      && context.scenarioTag != 0;
    return scope;
}

[[nodiscard]] std::string
row_label(const model::Node& node, HierarchyMode mode, const model::InspectionDocument& snapshot) {
    if (node.id == snapshot.graph.root()
        || (mode == HierarchyMode::source && node.kind == model::NodeKind::source)
        || (mode == HierarchyMode::activity && node.kind == model::NodeKind::activity)) {
        return root_label(mode, snapshot);
    }
    if (mode == HierarchyMode::source && node.kind == model::NodeKind::spawnSet
        && !node.source.mapStem.empty()) {
        return node.source.mapStem + " / " + node.name;
    }
    if (mode == HierarchyMode::activity && node.kind == model::NodeKind::spawnSet
        && node.source.activityIndex.has_value()) {
        std::array<char, 96> text{};
        const int written = std::snprintf(text.data(),
                                          text.size(),
                                          "Destination %d / %s",
                                          *node.source.activityIndex,
                                          node.name.c_str());
        if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
            return std::string(text.data(), static_cast<std::size_t>(written));
        }
    }
    return node.name;
}

[[nodiscard]] model::NodeId hierarchy_root(const model::InspectionDocument& snapshot) noexcept {
    if (g_state.hierarchyMode == HierarchyMode::world) {
        return snapshot.graph.root();
    }
    const model::NodeKind wanted = g_state.hierarchyMode == HierarchyMode::source
                                       ? model::NodeKind::source
                                       : model::NodeKind::activity;
    for (const model::Node& node : snapshot.graph.nodes()) {
        if (node.kind == wanted) {
            return node.id;
        }
    }
    return snapshot.graph.root();
}

[[nodiscard]] model::NodeId prepare_graph_admission() {
    const model::NodeId root = hierarchy_root(world());
    graph_admission::Result result{};
    if (g_state.graphScope == GraphScope::filteredHierarchy) {
        result = graph_admission::filtered_hierarchy(
            world().graph, root, g_state.admitted, g_state.collapsed, g_state.graphAdmitted);
    } else {
        const model::NodeId selected = g_state.selection.selected();
        const model::Node* node = world().graph.node(selected);
        if (node == nullptr || node->producer == model::Producer::activityCatalog
            || !g_state.admitted.contains(selected.value)) {
            result = graph_admission::filtered_hierarchy(
                world().graph, root, g_state.admitted, g_state.collapsed, g_state.graphAdmitted);
        } else {
            result = graph_admission::selected_neighborhood(world().graph,
                                                            selected,
                                                            g_state.admitted,
                                                            g_state.collapsed,
                                                            g_state.graphAdmitted);
        }
    }
    g_state.graphOmitted = result.omitted;
    return result.root ? result.root : root;
}

void append_rows(const model::Graph& graph,
                 model::NodeId root,
                 const std::unordered_set<std::uint64_t>& admitted,
                 bool searching,
                 const model::InspectionDocument& snapshot) {
    g_state.rowTraversal.clear();
    g_state.rowVisited.clear();
    g_state.rowTraversal.reserve(admitted.size());
    g_state.rowVisited.reserve(admitted.size());
    g_state.rowTraversal.push_back(TreeTraversal{root, 0});

    while (!g_state.rowTraversal.empty()) {
        const TreeTraversal current = g_state.rowTraversal.back();
        g_state.rowTraversal.pop_back();
        if (!current.id || !admitted.contains(current.id.value)
            || !g_state.rowVisited.insert(current.id.value).second) {
            continue;
        }
        const model::Node* node = graph.node(current.id);
        if (node == nullptr) {
            continue;
        }

        std::size_t admittedChildren = 0;
        for (const model::NodeId child : node->children) {
            admittedChildren += admitted.contains(child.value) ? 1U : 0U;
        }
        const bool hasChildren = admittedChildren != 0;
        std::string label = row_label(*node, g_state.hierarchyMode, snapshot);
        if (hasChildren) {
            label += "  [";
            label += std::to_string(admittedChildren);
            label += "]";
        }
        g_state.rows.push_back(TreeRow{current.id, std::move(label), current.depth, hasChildren});

        if (!searching && g_state.collapsed.contains(current.id.value)) {
            continue;
        }
        const std::uint8_t childDepth = current.depth == (std::numeric_limits<std::uint8_t>::max)()
                                            ? current.depth
                                            : static_cast<std::uint8_t>(current.depth + 1U);
        for (auto child = node->children.rbegin(); child != node->children.rend(); ++child) {
            if (admitted.contains(child->value)) {
                g_state.rowTraversal.push_back(TreeTraversal{*child, childDepth});
            }
        }
    }
}

void clear_selection() noexcept {
    g_state.selection.clear();
    g_state.selectedKey = {};
}

void restore_stable_state(bool addDefaultCollapses) {
    g_state.hidden.clear();
    std::erase_if(g_state.hiddenKeys,
                  [](const model::NodeKey& key) { return world().graph.node(key) == nullptr; });
    for (const model::NodeKey& key : g_state.hiddenKeys) {
        const model::NodeId id = world().graph.resolve(key);
        if (id) {
            g_state.hidden.insert(id.value);
        }
    }

    g_state.collapsed.clear();
    std::erase_if(g_state.collapsedKeys,
                  [](const model::NodeKey& key) { return world().graph.node(key) == nullptr; });
    for (const model::NodeKey& key : g_state.collapsedKeys) {
        const model::NodeId id = world().graph.resolve(key);
        if (id) {
            g_state.collapsed.insert(id.value);
        }
    }
    if (addDefaultCollapses) {
        for (const model::Node& node : world().graph.nodes()) {
            if (node.children.size() >= 256U) {
                g_state.collapsed.insert(node.id.value);
                g_state.collapsedKeys.insert(node.key);
            }
        }
    }

    const model::NodeId selected = world().graph.resolve(g_state.selectedKey);
    if (selected) {
        g_state.selection.select(selected);
    } else {
        clear_selection();
    }
}

void rebuild_rows() {
    const model::InspectionDocument& snapshot = world();
    const std::string queryText(g_state.search.data());
    if (g_state.rowsValid && g_state.cachedGeneration == snapshot.graph.generation()
        && g_state.cachedSearch == queryText && g_state.cachedMode == g_state.hierarchyMode
        && g_state.cachedHiddenRevision == g_state.hiddenRevision) {
        return;
    }

    g_state.rows.clear();
    g_state.admitted.clear();
    g_state.overviewAdmitted.clear();
    const model::Query query = model::parse_query(queryText);
    g_state.queryError = query.error;
    const bool searching = !query.terms.empty();
    g_state.admitted.reserve(snapshot.graph.nodes().size());
    g_state.overviewAdmitted.reserve(snapshot.graph.nodes().size());
    g_state.rows.reserve(snapshot.graph.nodes().size());

    for (const model::Node& node : snapshot.graph.nodes()) {
        if (node_admitted_by_filters(node, query)) {
            g_state.admitted.insert(node.id.value);
        }
        if (overview_node_admitted_by_filters(node, query)) {
            g_state.overviewAdmitted.insert(node.id.value);
        }
    }
    // Parents are always created before children. Walking the dense graph backwards propagates
    // every direct match to its complete ancestor chain in one pass, without repeatedly walking
    // the same chain for each matching descendant.
    for (auto node = snapshot.graph.nodes().rbegin(); node != snapshot.graph.nodes().rend();
         ++node) {
        if (g_state.admitted.contains(node->id.value) && node->parent
            && snapshot.graph.node(node->parent) != nullptr) {
            g_state.admitted.insert(node->parent.value);
        }
    }

    append_rows(snapshot.graph, hierarchy_root(snapshot), g_state.admitted, searching, snapshot);
    ++g_state.admissionRevision;

    g_state.cachedGeneration = snapshot.graph.generation();
    g_state.cachedSearch = queryText;
    g_state.cachedMode = g_state.hierarchyMode;
    g_state.cachedHiddenRevision = g_state.hiddenRevision;
    g_state.rowsValid = true;
}

[[nodiscard]] bool control_button(const char* label,
                                  const ImVec2& size,
                                  bool active,
                                  bool enabled,
                                  const char* unavailableTooltip = "") noexcept {
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    if (active && enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.22F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.32F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.42F));
    }
    const bool pressed = ImGui::Button(label, size);
    if (active && enabled) {
        ImGui::PopStyleColor(3);
    }
    if (!enabled) {
        ImGui::EndDisabled();
        if (unavailableTooltip[0] != '\0'
            && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", unavailableTooltip);
        }
    }
    return enabled && pressed;
}

[[nodiscard]] bool
chip(const char* label, bool active, bool enabled, const char* unavailableTooltip) noexcept {
    return control_button(label, {0.0F, control_height()}, active, enabled, unavailableTooltip);
}

[[nodiscard]] bool tab_button(const char* label, bool active, float width) noexcept {
    return control_button(label, {width, control_height()}, active, true);
}

void hierarchy_tabs() noexcept {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float available = (std::max)(1.0F, ImGui::GetContentRegionAvail().x - spacing * 2.0F);
    const float width = available / 3.0F;
    const auto tab = [width](const char* label, HierarchyMode mode) noexcept {
        if (tab_button(label, g_state.hierarchyMode == mode, width)) {
            g_state.hierarchyMode = mode;
            g_state.rowsValid = false;
        }
    };

    tab("World", HierarchyMode::world);
    ImGui::SameLine();
    tab("Source", HierarchyMode::source);
    ImGui::SameLine();
    tab("Activity", HierarchyMode::activity);
}

void quick_filters() noexcept {
    const ImGuiStyle& style = ImGui::GetStyle();
    // Chips flow onto extra rows instead of clipping away when the panel is narrow.
    // Call with the NEXT item's label right after an item is drawn: the next chip
    // shares the row only when its full width fits beside the one just drawn. Only
    // SameLine() is used, so ImGui's line bookkeeping stays consistent.
    const auto flowNext = [&style](const char* nextLabel) noexcept {
        const float nextWidth = ImGui::CalcTextSize(nextLabel).x + style.FramePadding.x * 2.0F;
        const float rowEnd = ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextWidth;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float rightEdge = cursor.x + ImGui::GetContentRegionAvail().x;
        if (rowEnd <= rightEdge) {
            ImGui::SameLine();
        }
    };
    const auto filterChip = [](const char* label, FilterGroup group, bool& value) noexcept {
        const bool present = filter_present(group);
        if (chip(label, value, present, "No matching objects are present in this snapshot.")) {
            value = !value;
            g_state.rowsValid = false;
            persist_layout();
        }
    };

    filterChip("Geometry", FilterGroup::geometry, g_state.showGeometry);
    flowNext("Entities");
    filterChip("Entities", FilterGroup::entities, g_state.showEntities);
    flowNext("Spawns");
    filterChip("Spawns", FilterGroup::spawns, g_state.showSpawns);
    flowNext("Logic");
    filterChip("Logic", FilterGroup::logic, g_state.showLogic);
    flowNext("Triggers");
    filterChip("Triggers", FilterGroup::triggers, g_state.showTriggers);
    flowNext("Audio");
    filterChip("Audio", FilterGroup::audio, g_state.showAudio);
    flowNext("Include hidden");
    if (chip("Include hidden", g_state.showHidden, true, "")) {
        g_state.showHidden = !g_state.showHidden;
        g_state.rowsValid = false;
    }
    flowNext("Errors only");
    if (chip("Errors only", g_state.errorsOnly, true, "")) {
        g_state.errorsOnly = !g_state.errorsOnly;
        g_state.rowsValid = false;
    }
    flowNext("Logic filters");
    if (ImGui::BeginMenu("Logic filters")) {
        const auto preset = [](const char* label, const char* query) {
            if (ImGui::MenuItem(label)) {
                std::snprintf(g_state.search.data(), g_state.search.size(), "%s", query);
                g_state.rowsValid = false;
            }
        };
        preset("Spawn rules", "role:spawn");
        preset("Squads", "role:squad");
        preset("Triggers", "role:trigger");
        preset("Spatial", "role:spatial");
        preset("Objectives", "role:objective");
        preset("Devices", "role:device");
        preset("Actions", "role:action");
        preset("Conditions", "role:condition");
        ImGui::Separator();
        preset("Placed only", "placement:yes");
        preset("Strong evidence only", "confidence:strong");
        preset("Has relationships", "relationship:yes");
        ImGui::Separator();
        if (ImGui::MenuItem("Clear search")) {
            g_state.search[0] = '\0';
            g_state.rowsValid = false;
        }
        ImGui::EndMenu();
    }
}

void draw_disclosure(const TreeRow& row, const ImVec2& minimum, float rowHeight) noexcept {
    if (!row.hasChildren) {
        ImGui::Dummy({scaled(14.0F), rowHeight});
        return;
    }
    ImGui::PushID(static_cast<int>(row.id.value));
    ImGui::InvisibleButton("##disclosure", {scaled(14.0F), rowHeight});
    const bool collapsed = g_state.collapsed.contains(row.id.value);
    const ImVec2 center{minimum.x + scaled(7.0F), minimum.y + rowHeight * 0.5F};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    if (collapsed) {
        const std::array<ImVec2, 3> points{
            ImVec2{center.x - 2.5F, center.y - 4.0F},
            ImVec2{center.x - 2.5F, center.y + 4.0F},
            ImVec2{center.x + 3.5F, center.y},
        };
        drawList->AddTriangleFilled(points[0], points[1], points[2], color);
    } else {
        const std::array<ImVec2, 3> points{
            ImVec2{center.x - 4.0F, center.y - 2.5F},
            ImVec2{center.x + 4.0F, center.y - 2.5F},
            ImVec2{center.x, center.y + 3.5F},
        };
        drawList->AddTriangleFilled(points[0], points[1], points[2], color);
    }
    if (ImGui::IsItemClicked()) {
        const model::NodeKey key = world().graph.key(row.id);
        if (collapsed) {
            g_state.collapsed.erase(row.id.value);
            g_state.collapsedKeys.erase(key);
        } else {
            g_state.collapsed.insert(row.id.value);
            g_state.collapsedKeys.insert(key);
        }
        g_state.rowsValid = false;
    }
    ImGui::PopID();
}

void expand_selected_ancestors() noexcept {
    model::NodeId cursor = g_state.selection.selected();
    std::size_t guard = 0;
    bool changed = false;
    while (cursor && guard++ <= world().graph.nodes().size()) {
        const model::Node* node = world().graph.node(cursor);
        if (node == nullptr || !node->parent) {
            break;
        }
        changed = g_state.collapsed.erase(node->parent.value) != 0 || changed;
        g_state.collapsedKeys.erase(world().graph.key(node->parent));
        cursor = node->parent;
    }
    if (changed) {
        g_state.rowsValid = false;
    }
}

model::NodeId draw_tree() noexcept {
    if (g_state.revealSelection && g_state.selection.selected() && g_state.search[0] == '\0') {
        expand_selected_ancestors();
    }
    rebuild_rows();
    model::NodeId hovered{};
    if (g_state.rows.empty()) {
        ImGui::TextDisabled("No objects match the current view.");
        return hovered;
    }

    const float rowHeight = padded_row_height(
        ImGui::GetTextLineHeight(), scaled(kInspectorRowPadding), scaled(kMinimumTreeRowHeight));
    bool scrollAdjusted = false;
    if (g_state.revealSelection && g_state.selection.selected()) {
        const auto iterator =
            std::find_if(g_state.rows.begin(), g_state.rows.end(), [](const TreeRow& row) {
                return row.id == g_state.selection.selected();
            });
        if (iterator != g_state.rows.end()) {
            const float index = static_cast<float>(iterator - g_state.rows.begin());
            const float target = index * rowHeight - ImGui::GetWindowHeight() * 0.45F;
            ImGui::SetScrollY(snap_scroll(target, rowHeight, ImGui::GetScrollMaxY()));
            scrollAdjusted = true;
        }
        g_state.revealSelection = false;
        g_state.restoreTreeScroll = false;
    } else if (g_state.restoreTreeScroll && g_state.treeAnchor) {
        const auto iterator = std::ranges::find_if(g_state.rows, [](const TreeRow& row) {
            const model::Node* node = world().graph.node(row.id);
            return node != nullptr && identity_matches(g_state.treeAnchor, *node);
        });
        if (iterator != g_state.rows.end()) {
            const float index = static_cast<float>(iterator - g_state.rows.begin());
            ImGui::SetScrollY(snap_scroll(index * rowHeight, rowHeight, ImGui::GetScrollMaxY()));
            scrollAdjusted = true;
        }
        g_state.restoreTreeScroll = false;
    } else {
        const float scrollY = ImGui::GetScrollY();
        const float snapped = snap_scroll(scrollY, rowHeight, ImGui::GetScrollMaxY());
        if (std::abs(snapped - scrollY) > 0.01F) {
            ImGui::SetScrollY(snapped);
            scrollAdjusted = true;
        }
    }

    const ImVec2 itemSpacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {itemSpacing.x, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0F, 0.5F});
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_state.rows.size()), rowHeight);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const TreeRow& row = g_state.rows[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            if (!fully_visible(rowStart, {rowStart.x, rowStart.y + rowHeight})) {
                ImGui::Dummy({0.0F, rowHeight});
                ImGui::PopID();
                continue;
            }
            const float indent = scaled(kTreeIndent) * static_cast<float>(row.depth);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            for (std::uint8_t depth = 0; depth < row.depth; ++depth) {
                const float x =
                    rowStart.x + scaled(kTreeIndent) * (static_cast<float>(depth) + 0.5F);
                drawList->AddLine({x, rowStart.y}, {x, rowStart.y + rowHeight}, kGuideColor);
            }
            ImGui::SetCursorScreenPos({rowStart.x + indent, rowStart.y});
            draw_disclosure(row, {rowStart.x + indent, rowStart.y}, rowHeight);
            ImGui::SameLine(0.0F, 1.0F);

            const bool selected = g_state.selection.selected() == row.id;
            const model::Node* node = world().graph.node(row.id);
            const bool helper =
                node != nullptr && model::supports(node->actions, model::Action::hide);
            if (helper) {
                ImGui::PushStyleColor(ImGuiCol_Text, hidden(row.id) ? kMuted : kSpawn);
            }
            const float textWidth = ImGui::CalcTextSize(row.label.c_str()).x;
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float itemWidth = (std::max)(1.0F, availableWidth);
            if (ImGui::Selectable(row.label.c_str(),
                                  selected,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  {itemWidth, rowHeight})) {
                select_node(row.id);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    focus(row.id);
                }
            }
            if (helper) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) {
                hovered = row.id;
                if (textWidth > availableWidth) {
                    ImGui::SetTooltip("%s", row.label.c_str());
                }
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    select_node(row.id);
                    g_state.contextTarget = row.id;
                    g_state.contextRequested = true;
                }
            }
            ImGui::PopID();
        }
    }
    const ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float visibleHeight =
        (std::max)(0.0F, drawList->GetClipRectMax().y - drawList->GetClipRectMin().y);
    const float contentHeight = static_cast<float>(g_state.rows.size()) * rowHeight;
    const float endPadding = aligned_scroll_padding(contentHeight, visibleHeight, rowHeight);
    if (endPadding > 0.0F) {
        ImGui::Dummy({0.0F, endPadding});
    }
    ImGui::PopStyleVar(2);
    if (!scrollAdjusted) {
        const float scrollY = ImGui::GetScrollY();
        const std::size_t anchorIndex =
            (std::min)(static_cast<std::size_t>((std::max)(0.0F, scrollY) / rowHeight),
                       g_state.rows.size() - 1U);
        const model::Node* anchorNode = world().graph.node(g_state.rows[anchorIndex].id);
        g_state.treeAnchor = node_identity(anchorNode);
        g_state.treeAnchorOffset = 0.0F;
    }
    return hovered;
}

void draw_outliner() noexcept {
    section::header("Scene tree");
    hierarchy_tabs();
    if (g_state.focusSearch) {
        ImGui::SetKeyboardFocusHere();
        g_state.focusSearch = false;
    }
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint("##world_search",
                                 "Search or type:spawn  tag:80806730",
                                 g_state.search.data(),
                                 g_state.search.size())) {
        g_state.rowsValid = false;
    }
    quick_filters();
    if (!g_state.queryError.empty()) {
        ImGui::TextColored(kFailure, "%s", g_state.queryError.c_str());
    }
    ImGui::Separator();

    if (control_button("Expand all", {0.0F, control_height()}, false, true)) {
        g_state.collapsed.clear();
        g_state.collapsedKeys.clear();
        g_state.rowsValid = false;
    }
    ImGui::SameLine();
    if (control_button("Collapse all", {0.0F, control_height()}, false, true)) {
        g_state.collapsed.clear();
        g_state.collapsedKeys.clear();
        for (const model::Node& node : world().graph.nodes()) {
            if (!node.children.empty()) {
                g_state.collapsed.insert(node.id.value);
                g_state.collapsedKeys.insert(node.key);
            }
        }
        g_state.rowsValid = false;
    }
    ImGui::Separator();

    ImGui::BeginChild("##scene_tree_scroll", {0.0F, 0.0F}, false);
    const model::NodeId hovered = draw_tree();
    if (hovered) {
        g_state.selection.hover(hovered);
    }
    ImGui::EndChild();
}

[[nodiscard]] bool inspector_header(const char* label, ImGuiTreeNodeFlags flags) noexcept {
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    const bool visible = next_item_fully_visible(ImGui::GetTextLineHeight() + padding.y * 2.0F);
    if (!visible) {
        ImGui::BeginDisabled();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0F);
    }
    const bool open = ImGui::CollapsingHeader(label, flags);
    if (!visible) {
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
    }
    ImGui::PopStyleVar();
    return open;
}

void inspector_text(const char* text, bool disabled = false) noexcept {
    const float height = ImGui::GetTextLineHeight();
    if (!next_item_fully_visible(height)) {
        ImGui::Dummy({0.0F, height});
        return;
    }
    if (disabled) {
        ImGui::TextDisabled("%s", text);
    } else {
        ImGui::TextUnformatted(text);
    }
}

void inspector_title(const model::Node& node) noexcept {
    const float height = ImGui::GetTextLineHeight();
    if (!next_item_fully_visible(height)) {
        ImGui::Dummy({0.0F, height});
        return;
    }
    ImGui::TextUnformatted(node.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%s", model::kind_name(node.kind));
}

[[nodiscard]] bool inspector_checkbox(const char* label, bool& value) noexcept {
    const float height = ImGui::GetFrameHeight();
    if (next_item_fully_visible(height)) {
        return ImGui::Checkbox(label, &value);
    }
    const float width =
        height + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
    ImGui::Dummy({width, height});
    return false;
}

void property_row(const char* name, const char* value) noexcept {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const ImVec2 contentMinimum = ImGui::GetCursorScreenPos();
    const float fieldWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);
    const float fieldHeight = ImGui::CalcTextSize(name, nullptr, false, fieldWidth).y;
    ImGui::TableSetColumnIndex(1);
    const float wrapWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);
    const float valueHeight = ImGui::CalcTextSize(value, nullptr, false, wrapWidth).y;
    const float contentHeight = (std::max)({ImGui::GetTextLineHeight(), fieldHeight, valueHeight});
    const float padding = ImGui::GetStyle().CellPadding.y;
    const ImVec2 rowMinimum{contentMinimum.x, contentMinimum.y - padding};
    const ImVec2 rowMaximum{contentMinimum.x, contentMinimum.y + contentHeight + padding};
    const bool visible = fully_visible(rowMinimum, rowMaximum);
    if (!visible) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 0, 0, 0));
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(0, 0, 0, 0));
    }
    ImGui::TableSetColumnIndex(0);
    if (visible) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", name);
        ImGui::PopStyleColor();
    } else {
        ImGui::Dummy({0.0F, contentHeight});
    }
    ImGui::TableSetColumnIndex(1);
    if (visible) {
        ImGui::TextWrapped("%s", value);
    } else {
        ImGui::Dummy({0.0F, contentHeight});
    }
}

void property_u64(const char* name, std::uint64_t value, int width = 16) noexcept {
    std::array<char, 32> text{};
    (void)std::snprintf(
        text.data(), text.size(), "0x%0*llX", width, static_cast<unsigned long long>(value));
    property_row(name, text.data());
}

void property_i32(const char* name, std::int32_t value) noexcept {
    std::array<char, 24> text{};
    (void)std::snprintf(text.data(), text.size(), "%d", value);
    property_row(name, text.data());
}

[[nodiscard]] bool begin_properties(const char* id) noexcept {
    if (!ImGui::BeginTable(id,
                           2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
                               | ImGuiTableFlags_BordersInnerH)) {
        return false;
    }
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, scaled(112.0F));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void end_properties() noexcept {
    ImGui::EndTable();
}

void draw_identity(const model::Node& node) noexcept {
    if (!inspector_header("Identity", ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##identity_properties")) {
        return;
    }
    property_u64("Inspection ID", node.id.value);
    property_row("Producer", model::producer_name(node.producer));
    property_row("Provenance", model::provenance_name(node.provenance));
    property_u64("Native key", capture::stable_native_key(world(), node));
    property_u64("Graph generation", world().graph.generation(), 8);
    const std::string stableIdentity = capture::stable_identity(world(), node);
    property_row("Stable identity", stableIdentity.c_str());
    if (node.parent) {
        property_u64("Parent", node.parent.value);
    }
    property_row("Type", model::kind_name(node.kind));
    property_row("Status", model::status_name(node.status));
    if (node.runtimeEntity.has_value()) {
        property_u64("Runtime entity", *node.runtimeEntity);
    }
    if (node.objectSystemType.has_value()) {
        property_u64("Object-system type", *node.objectSystemType, 2);
    }
    if (node.observationId.has_value()) {
        property_u64("Observation slot", *node.observationId);
    }
    if (node.triggerSourceHash.has_value()) {
        property_u64("Trigger source", *node.triggerSourceHash, 8);
    }
    if (node.triggerOverlapCount.has_value()) {
        property_i32("Overlapping bodies", static_cast<std::int32_t>(*node.triggerOverlapCount));
    }
    if (node.triggerSelector.has_value()) {
        property_i32("Event selector", *node.triggerSelector);
    }
    if (node.triggerEnabled.has_value()) {
        property_row("Enabled", *node.triggerEnabled ? "Yes" : "No");
    }
    if (node.triggerActive.has_value()) {
        property_row("Active", *node.triggerActive ? "Yes" : "No");
    }
    if (node.worldId.has_value()) {
        property_u64("WorldID", *node.worldId);
    }
    if (node.tag.has_value()) {
        property_u64("Tag hash", *node.tag, 8);
    }
    if (node.classHash.has_value()) {
        property_u64("Class hash", *node.classHash, 8);
    }
    if (node.nameHash.has_value()) {
        property_u64("Name hash", *node.nameHash, 8);
    }
    property_i32("Children", static_cast<std::int32_t>(node.children.size()));
    end_properties();
}

void draw_transform(const model::Node& node) noexcept {
    if (!node.transform.has_value()
        || !inspector_header("Transform", ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##transform_properties")) {
        return;
    }

    const auto formatVector = [](const std::array<float, 3>& vector,
                                 std::array<char, 96>& text) noexcept {
        (void)std::snprintf(text.data(),
                            text.size(),
                            "%.4f, %.4f, %.4f",
                            static_cast<double>(vector[0]),
                            static_cast<double>(vector[1]),
                            static_cast<double>(vector[2]));
    };

    std::array<char, 96> text{};
    formatVector(node.transform->position, text);
    property_row("Position", text.data());
    if (node.transform->hasRotation) {
        formatVector(node.transform->rotation, text);
        property_row("Rotation", text.data());
    }
    if (node.transform->hasScale) {
        formatVector(node.transform->scale, text);
        property_row("Scale", text.data());
    }
    if (node.linearVelocity.has_value()) {
        formatVector(*node.linearVelocity, text);
        property_row("Linear velocity", text.data());
    }
    end_properties();
}

void draw_bounds(const model::Node& node) noexcept {
    if (!node.bounds.has_value()) {
        if (node.kind == model::NodeKind::trigger && node.transform.has_value()) {
            if (inspector_header("Bounds", ImGuiTreeNodeFlags_DefaultOpen)) {
                inspector_text("Shape unavailable; center observation only", true);
            }
        }
        return;
    }
    if (!model::bounds_valid(*node.bounds)) {
        if (inspector_header("Bounds", ImGuiTreeNodeFlags_DefaultOpen)) {
            inspector_text("Invalid bounds were rejected by the inspection model.", true);
        }
        return;
    }
    if (!inspector_header("Bounds", ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##bounds_properties")) {
        return;
    }
    std::array<char, 96> text{};
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(node.bounds->minimum[0]),
                        static_cast<double>(node.bounds->minimum[1]),
                        static_cast<double>(node.bounds->minimum[2]));
    property_row("Minimum", text.data());
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(node.bounds->maximum[0]),
                        static_cast<double>(node.bounds->maximum[1]),
                        static_cast<double>(node.bounds->maximum[2]));
    property_row("Maximum", text.data());
    const auto center = model::bounds_center(*node.bounds);
    const auto extents = model::bounds_extents(*node.bounds);
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(center[0]),
                        static_cast<double>(center[1]),
                        static_cast<double>(center[2]));
    property_row("Center", text.data());
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(extents[0]),
                        static_cast<double>(extents[1]),
                        static_cast<double>(extents[2]));
    property_row("Half extents", text.data());
    property_row("Provenance",
                 node.boundsProvenance.has_value() ? model::provenance_name(*node.boundsProvenance)
                                                   : "unknown");
    end_properties();
}

void draw_source(const model::Node& node) noexcept {
    const model::Source& source = node.source;
    if (source.packageName.empty() && source.mapStem.empty() && !source.scenarioTag.has_value()
        && !source.spawnSetHash.has_value() && !source.bubble.has_value()) {
        return;
    }
    if (!inspector_header("Source", ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##source_properties")) {
        return;
    }
    if (!source.packageName.empty()) {
        property_row("Package", source.packageName.c_str());
    }
    if (!source.mapStem.empty()) {
        property_row("Map stem", source.mapStem.c_str());
    }
    if (source.scenarioTag.has_value()) {
        property_u64("Scenario tag", *source.scenarioTag, 8);
    }
    if (source.spawnSetHash.has_value()) {
        property_u64("Spawn set", *source.spawnSetHash, 8);
    }
    if (source.bubble.has_value()) {
        property_i32("Bubble", *source.bubble);
    }
    if (source.authoredPreview) {
        property_row("Context", "Authored preview - not live");
    }
    end_properties();
}

void draw_activity(const model::Node& node) noexcept {
    const model::Source& source = node.source;
    if (!source.activitySession.has_value() && !source.activityIndex.has_value()) {
        return;
    }
    if (!inspector_header("Activity", ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##activity_properties")) {
        return;
    }
    if (source.activitySession.has_value()) {
        property_u64("Session", *source.activitySession);
    }
    if (source.activityIndex.has_value()) {
        property_i32("Definition index", *source.activityIndex);
    }
    property_row("Relationship",
                 source.spawnSetHash.has_value() ? "Destination spawn-set hash" : "Unknown");
    end_properties();
}

[[nodiscard]] model::NodeId logic_node_for_scenario(std::uint32_t scenarioTag) noexcept;
[[nodiscard]] std::vector<std::uint32_t> activity_graphs_for_scenario(std::uint32_t scenarioTag);

void draw_activity_metadata(const model::Node& node) noexcept {
    if (!node.activityMetadata.has_value()) {
        return;
    }
    const model::ActivityMetadata& metadata = *node.activityMetadata;
    if (!inspector_header("Activity catalog metadata", ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##activity_catalog_properties")) {
        return;
    }
    property_u64("Activity hash", metadata.activityHash, 8);
    property_u64("Graph hash", metadata.graphHash, 8);
    property_u64("Node hash", metadata.nodeHash, 8);
    if (!metadata.nativeStateValues.empty()) {
        std::string states;
        for (std::size_t index = 0; index < metadata.nativeStateValues.size(); ++index) {
            if (index != 0) {
                states.append(", ");
            }
            states.append(std::to_string(metadata.nativeStateValues[index]));
        }
        property_row("Native state sequence", states.c_str());
    }
    std::array<char, 96> position{};
    std::snprintf(position.data(),
                  position.size(),
                  "%.3f, %.3f",
                  static_cast<double>(metadata.authoredPosition[0]),
                  static_cast<double>(metadata.authoredPosition[1]));
    property_row("Authored position", position.data());
    property_u64("Catalog build", metadata.catalogBuild, 8);
    property_i32("Collector version", static_cast<std::int32_t>(metadata.collectorVersion));
    property_i32("Activity references", static_cast<std::int32_t>(metadata.referenceCount));
    end_properties();

    const bool exactLogicLink = metadata.activityHash != 0 && node.source.scenarioTag.has_value()
                                && metadata.activityHash == *node.source.scenarioTag;
    if (exactLogicLink) {
        ImGui::TextUnformatted("Exact Activity Logic link");
        const model::NodeId logicNode = logic_node_for_scenario(metadata.activityHash);
        if (logicNode) {
            if (ImGui::Button("Open Activity Logic")) {
                select_node(logicNode);
            }
        } else {
            ImGui::TextDisabled("No matching current-location Activity Logic node is active.");
        }
    }

    if (metadata.linkedGraphHashes.empty()) {
        return;
    }
    ImGui::TextUnformatted("Linked graphs");
    for (const std::uint32_t graphHash : metadata.linkedGraphHashes) {
        std::array<char, 64> label{};
        std::snprintf(label.data(), label.size(), "Open graph 0x%08X", graphHash);
        ImGui::PushID(static_cast<int>(graphHash));
        if (ImGui::Button(label.data())) {
            open_activity_graph(graphHash);
        }
        ImGui::PopID();
    }
}

[[nodiscard]] model::NodeId logic_node_for_definition(std::uint32_t definitionTag) noexcept {
    for (const model::Node& node : world().graph.nodes()) {
        if (node.kind == model::NodeKind::logicEntity && node.activityLogicMetadata.has_value()
            && node.activityLogicMetadata->definitionTag == definitionTag) {
            return node.id;
        }
    }
    return {};
}

[[nodiscard]] model::NodeId logic_node_for_scenario(std::uint32_t scenarioTag) noexcept {
    for (const model::Node& node : world().graph.nodes()) {
        if (node.kind == model::NodeKind::activityLogic
            && node.producer == model::Producer::activityLogicCatalog
            && node.nativeKey == scenarioTag) {
            return node.id;
        }
    }
    return {};
}

[[nodiscard]] std::vector<std::uint32_t> activity_graphs_for_scenario(std::uint32_t scenarioTag) {
    std::vector<std::uint32_t> matches;
    for (const model::Node& node : world().graph.nodes()) {
        if (node.producer == model::Producer::activityCatalog
            && node.kind == model::NodeKind::activityGraph
            && node.source.scenarioTag.has_value() && *node.source.scenarioTag == scenarioTag
            && node.activityMetadata.has_value()
            && node.activityMetadata->graphHash != 0) {
            matches.push_back(node.activityMetadata->graphHash);
        }
    }
    std::ranges::sort(matches);
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

void draw_activity_logic_metadata(const model::Node& node) noexcept {
    if (!node.activityLogicMetadata.has_value()) {
        return;
    }
    const model::ActivityLogicMetadata& metadata = *node.activityLogicMetadata;
    if (inspector_header("Activity logic", ImGuiTreeNodeFlags_DefaultOpen)
        && begin_properties("##activity_logic_properties")) {
        property_u64("Scenario", metadata.scenarioTag, 8);
        property_u64("Definition", metadata.definitionTag, 8);
        property_u64("Class primary", metadata.classPrimary, 8);
        property_u64("Class secondary", metadata.classSecondary, 8);
        property_row("Role", metadata.roleName.c_str());
        property_row("Label", metadata.label.c_str());
        property_row("Confidence", metadata.confidenceName.c_str());
        if (!metadata.localizedText.empty()) {
            property_row("Localized text", metadata.localizedText.c_str());
        }
        property_i32("Authored placements", static_cast<std::int32_t>(metadata.placementCount));
        if (metadata.hasPlacement) {
            property_u64("WorldID", metadata.worldId);
            property_u64("Map table", metadata.mapTableTag, 8);
            property_u64("Placed entity", metadata.placedEntityTag, 8);
            std::array<char, 128> rotation{};
            std::snprintf(rotation.data(),
                          rotation.size(),
                          "%.4f, %.4f, %.4f, %.4f",
                          static_cast<double>(metadata.authoredRotation[0]),
                          static_cast<double>(metadata.authoredRotation[1]),
                          static_cast<double>(metadata.authoredRotation[2]),
                          static_cast<double>(metadata.authoredRotation[3]));
            property_row("Authored quaternion", rotation.data());
        }
        end_properties();
    }
    if (metadata.scenarioTag != 0) {
        const std::vector<std::uint32_t> graphHashes =
            activity_graphs_for_scenario(metadata.scenarioTag);
        ImGui::TextUnformatted("Exact Activity Map links");
        if (graphHashes.empty()) {
            ImGui::TextDisabled("No exact Activity Map activity hash is present in this snapshot.");
        } else if (graphHashes.size() == 1) {
            if (ImGui::Button("Open in Activity Map")) {
                open_activity_graph(graphHashes.front());
            }
        } else {
            ImGui::TextDisabled("%zu exact graph matches; choose one:", graphHashes.size());
            for (const std::uint32_t graphHash : graphHashes) {
                std::array<char, 64> label{};
                std::snprintf(label.data(), label.size(), "Open graph 0x%08X", graphHash);
                ImGui::PushID(static_cast<int>(graphHash));
                if (ImGui::Button(label.data())) {
                    open_activity_graph(graphHash);
                }
                ImGui::PopID();
            }
        }
    }
    if ((metadata.roleName.find("Trigger") != std::string::npos
         || metadata.roleName.find("Spatial") != std::string::npos)
        && !node.transform.has_value()) {
        inspector_text("Static archive identifies spatial/volume logic, but this definition has no "
                       "proven shape or world transform.",
                       true);
    }
    if (metadata.relationships.empty()) {
        return;
    }
    if (ImGui::Button("Open serialized-reference graph")) {
        g_state.centerMode = CenterMode::relationships;
        g_state.relationshipGraphState.fitRequested = true;
    }
    ImGui::TextUnformatted("Serialized name references (not execution flow)");
    std::size_t relationshipIndex = 0;
    for (const model::ActivityLogicRelationship& relationship : metadata.relationships) {
        std::array<char, 96> label{};
        std::snprintf(label.data(),
                      label.size(),
                      "%s 0x%08X  hash 0x%08X  x%u",
                      relationship.outgoing ? "Source contains" : "Referenced by",
                      relationship.definitionTag,
                      relationship.nameHash,
                      relationship.occurrenceCount);
        ImGui::PushID(static_cast<int>(relationshipIndex++));
        ImGui::PushID(static_cast<int>(relationship.definitionTag));
        ImGui::PushID(static_cast<int>(relationship.nameHash));
        ImGui::TextDisabled("%s", relationship.outgoing ? "->" : "<-");
        ImGui::SameLine();
        const model::NodeId target = logic_node_for_definition(relationship.definitionTag);
        ImGui::BeginDisabled(!target);
        if (ImGui::Button(label.data())) {
            select_node(target);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }
}

void draw_rendering(const model::Node& node) noexcept {
    if (!model::supports(node.actions, model::Action::hide)
        || !inspector_header("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    bool visible = !hidden(node.id);
    if (inspector_checkbox("Inspector helper visible", visible)) {
        toggle_hidden(node.id);
    }
    inspector_text("Game-render visibility is unavailable.", true);
}

void draw_state_variable_or_root(const model::Node& node) noexcept {
    if (node.kind != model::NodeKind::logicVariable
        && !(node.kind == model::NodeKind::logicGroup && node.classHash.has_value()
             && *node.classHash == 0x8080941EU)) {
        return;
    }
    const char* title = node.kind == model::NodeKind::logicVariable ? "State variable"
                                                                     : "Behavior root";
    if (!inspector_header(title, ImGuiTreeNodeFlags_DefaultOpen)
        || !begin_properties("##authored_logic_node_properties")) {
        return;
    }
    if (node.tag.has_value()) {
        property_u64(node.kind == model::NodeKind::logicVariable ? "Config tag" : "Root tag",
                     *node.tag,
                     8);
    }
    if (node.nameHash.has_value()) {
        property_u64("Name hash", *node.nameHash, 8);
    }
    if (node.classHash.has_value()) {
        property_u64("Class", *node.classHash, 8);
    }
    for (const model::Property& property : node.properties) {
        if (!property.visible) {
            continue;
        }
        std::visit(
            [&property](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, bool>) {
                    property_row(property.label.c_str(), value ? "true" : "false");
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    property_row(property.label.c_str(), value.c_str());
                } else if constexpr (std::is_integral_v<Value>) {
                    property_i32(property.label.c_str(), static_cast<std::int32_t>(value));
                }
            },
            property.value);
    }
    end_properties();
}

void draw_inspector_actions(const model::Node& node) noexcept {
    const camera::Status status = camera::status();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float buttonWidth = (std::max)(1.0F, (ImGui::GetContentRegionAvail().x - spacing) * 0.5F);
    bool secondColumn = false;

    const auto button = [&secondColumn, buttonWidth](const char* label) noexcept {
        if (secondColumn) {
            ImGui::SameLine();
        }
        secondColumn = !secondColumn;
        const float buttonHeight = control_height();
        if (!next_item_fully_visible(buttonHeight)) {
            ImGui::Dummy({buttonWidth, buttonHeight});
            return false;
        }
        return ImGui::Button(label, {buttonWidth, buttonHeight});
    };

    const CameraTeleportAvailability cameraTeleport = camera_teleport_availability(node.id, status);
    ImGui::BeginDisabled(!cameraTeleport.available());
    if (button("Focus camera")) {
        focus(node.id);
    }
    ImGui::EndDisabled();
    if (!cameraTeleport.available() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Unavailable: %s", cameraTeleport.unavailableReason);
    }
    if (model::supports(node.actions, model::Action::hide)
        && button(hidden(node.id) ? "Show" : "Hide")) {
        toggle_hidden(node.id);
    }
    if (model::supports(node.actions, model::Action::isolate) && button("Isolate")) {
        isolate(node.id);
    }
    if (model::supports(node.actions, model::Action::copyId) && button("Copy ID")) {
        copy_id(node.id);
    }
    if (button("Copy stable ID")) {
        copy_text(capture::stable_identity(world(), node));
    }
    if (button("Copy path")) {
        copy_text(world().graph.breadcrumb(node.id));
    }
    if (node.tag.has_value() && model::supports(node.actions, model::Action::copyTag)
        && button("Copy tag")) {
        copy_tag(*node.tag);
    }
    if (node.transform.has_value() && model::supports(node.actions, model::Action::copyPosition)
        && button("Copy position")) {
        copy_position(*node.transform);
    }
}

void draw_inspector() noexcept {
    section::header("Selection Details");
    const model::Node* node = selected_node();
    if (node == nullptr) {
        ImGui::Separator();
        ImGui::TextDisabled("Select an object in the Scene Tree or viewport.");
        return;
    }

    // Wrap long source paths within the inspector width.
    ImGui::Spacing();
    {
        const std::string breadcrumb = world().graph.breadcrumb(node->id);
        const float wrapWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);
        const float textHeight =
            ImGui::CalcTextSize(breadcrumb.c_str(), nullptr, false, wrapWidth).y;
        if (!next_item_fully_visible(textHeight)) {
            ImGui::Dummy({0.0F, textHeight});
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", breadcrumb.c_str());
            ImGui::PopStyleColor();
        }
    }
    inspector_title(*node);
    draw_inspector_actions(*node);
    ImGui::Separator();
    draw_identity(*node);
    draw_transform(*node);
    draw_bounds(*node);
    draw_rendering(*node);
    draw_activity(*node);
    draw_source(*node);
    draw_activity_metadata(*node);
    draw_activity_logic_metadata(*node);
    draw_state_variable_or_root(*node);
}

void graph_reference_row(const char* relation, const model::Node& node) noexcept {
    std::string label;
    label.reserve(node.name.size() + 48);
    label.append(relation);
    label.append("  ");
    label.append(node.name);
    label.append("  [");
    label.append(model::kind_name(node.kind));
    label.push_back(']');

    ImGui::PushID(static_cast<int>(node.id.value));
    if (ImGui::Selectable(label.c_str(),
                          g_state.selection.selected() == node.id,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
        select_node(node.id);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            focus(node.id);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", world().graph.breadcrumb(node.id).c_str());
    }
    ImGui::PopID();
}

void draw_references() noexcept {
    const model::Node* node = selected_node();
    if (node == nullptr) {
        ImGui::TextDisabled("Select an object to inspect graph relationships.");
        return;
    }

    ImGui::TextUnformatted("Ownership relationships");
    ImGui::TextDisabled(
        "Parent and child rows are the pointer-free inspection ownership hierarchy.");
    ImGui::Spacing();

    ImGui::TextUnformatted("Parent");
    if (node->parent) {
        if (const model::Node* parent = world().graph.node(node->parent); parent != nullptr) {
            graph_reference_row("parent", *parent);
        } else {
            ImGui::TextDisabled("Parent is no longer present in this snapshot.");
        }
    } else {
        ImGui::TextDisabled("Root node");
    }

    ImGui::Spacing();
    ImGui::Text("Children (%zu)", node->children.size());
    if (node->children.empty()) {
        ImGui::TextDisabled("No graph children.");
    } else {
        for (const model::NodeId childId : node->children) {
            if (const model::Node* child = world().graph.node(childId); child != nullptr) {
                graph_reference_row("child", *child);
            }
        }
    }

    if (!node->relations.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Typed non-ownership relationships (%zu)", node->relations.size());
        ImGui::TextDisabled(
            "Static catalog evidence; these links do not prove that an encounter is live.");
        if (control_button("Open relationship graph", {0.0F, control_height()}, false, true)) {
            g_state.centerMode = CenterMode::relationships;
            g_state.relationshipGraphState.fitRequested = true;
        }
        std::size_t index = 0;
        for (const model::Relation& relationship : node->relations) {
            std::array<char, 160> label{};
            const model::Node* target = world().graph.node(relationship.target);
            const char* relationKind = relationship.kind == model::RelationKind::logicVariableRead
                                           ? "read"
                                       : relationship.kind == model::RelationKind::logicVariableWrite
                                           ? "write"
                                       : relationship.kind == model::RelationKind::authoredLink
                                           ? "owner binding"
                                           : "relationship";
            if (relationship.selector >= 0) {
                std::snprintf(label.data(),
                              label.size(),
                              "%s %s  %s  hash 0x%08X selector %d x%u",
                              relationKind,
                              relationship.outgoing ? "->" : "<-",
                              target == nullptr ? "target unavailable" : target->name.c_str(),
                              relationship.nameHash,
                              relationship.selector,
                              relationship.occurrenceCount);
            } else {
                std::snprintf(label.data(),
                              label.size(),
                              "%s %s  %s  x%u",
                              relationKind,
                              relationship.outgoing ? "->" : "<-",
                              target == nullptr ? "target unavailable" : target->name.c_str(),
                              relationship.occurrenceCount);
            }
            ImGui::PushID(static_cast<int>(index++));
            ImGui::BeginDisabled(target == nullptr);
            if (ImGui::Selectable(label.data(), false)) {
                select_node(target->id);
            }
            ImGui::EndDisabled();
            if (target == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(
                    "The target definition is not materialized in the current activity graph.");
            }
            ImGui::PopID();
        }
    }

    const model::Source& source = node->source;
    if (source.scenarioTag.has_value() || source.spawnSetHash.has_value()
        || source.activitySession.has_value()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Provenance Keys");
        if (source.scenarioTag.has_value()) {
            ImGui::Text("Scenario tag  0x%08X", *source.scenarioTag);
        }
        if (source.spawnSetHash.has_value()) {
            ImGui::Text("Spawn set     0x%08X", *source.spawnSetHash);
        }
        if (source.activitySession.has_value()) {
            ImGui::Text("Activity      0x%016llX",
                        static_cast<unsigned long long>(*source.activitySession));
        }
    }
}

[[nodiscard]] ImVec4 provenance_color(const char* origin) noexcept {
    return std::string_view(origin) == "runtime"   ? kSpawn
           : std::string_view(origin) == "catalog" ? kSelection
                                                   : kMuted;
}

void data_row(const char* field,
              const char* type,
              const char* value,
              const char* origin = "derived") noexcept {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(field);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", type);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
    ImGui::TableNextColumn();
    ImGui::TextColored(provenance_color(origin), "%s", origin);
}

void data_u64(const char* field,
              const char* type,
              std::uint64_t value,
              int width,
              const char* origin) noexcept {
    std::array<char, 32> text{};
    (void)std::snprintf(
        text.data(), text.size(), "0x%0*llX", width, static_cast<unsigned long long>(value));
    data_row(field, type, text.data(), origin);
}

void data_i32(const char* field,
              const char* type,
              std::int32_t value,
              const char* origin) noexcept {
    std::array<char, 32> text{};
    (void)std::snprintf(text.data(), text.size(), "%d", value);
    data_row(field, type, text.data(), origin);
}

void data_vec3(const char* field, const std::array<float, 3>& value, const char* origin) noexcept {
    std::array<char, 96> text{};
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(value[0]),
                        static_cast<double>(value[1]),
                        static_cast<double>(value[2]));
    data_row(field, "vec3", text.data(), origin);
}

void data_property(const model::Property& property) noexcept {
    std::array<char, 128> field{};
    (void)std::snprintf(field.data(), field.size(), "property.%s", property.key.c_str());
    std::string type;
    std::string value;
    std::visit(
        [&type, &value](const auto& typed) {
            using Value = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, bool>) {
                type = "bool";
                value = typed ? "true" : "false";
            } else if constexpr (std::is_same_v<Value, std::string>) {
                type = "string";
                value = typed;
            } else if constexpr (std::is_same_v<Value, std::array<float, 3>>) {
                type = "vec3";
                std::array<char, 96> text{};
                (void)std::snprintf(text.data(),
                                    text.size(),
                                    "%.4f, %.4f, %.4f",
                                    static_cast<double>(typed[0]),
                                    static_cast<double>(typed[1]),
                                    static_cast<double>(typed[2]));
                value = text.data();
            } else if constexpr (std::is_floating_point_v<Value>) {
                type = "number";
                value = std::to_string(typed);
            } else {
                type = std::is_signed_v<Value> ? "i64" : "u64";
                value = std::to_string(typed);
            }
        },
        property.value);
    data_row(
        field.data(), type.c_str(), value.c_str(), model::provenance_name(property.provenance));
}

void draw_data() noexcept {
    const model::Node* node = selected_node();
    if (node == nullptr) {
        ImGui::TextDisabled("Select an object to inspect structural data.");
        return;
    }
    if (!ImGui::BeginTable("##data_table",
                           4,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner
                               | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Origin");
    ImGui::TableHeadersRow();

    data_u64("inspection_id", "u64", node->id.value, 16, "derived");
    data_row("producer", "enum", model::producer_name(node->producer), "derived");
    data_row("provenance", "enum", model::provenance_name(node->provenance), "derived");
    data_u64("native_key", "u64", capture::stable_native_key(world(), *node), 16, "derived");
    const std::string stableIdentity = capture::stable_identity(world(), *node);
    data_row("stable_identity", "string", stableIdentity.c_str(), "derived");
    if (node->parent) {
        data_u64("parent_id", "u64", node->parent.value, 16, "derived");
    }
    data_row("name", "string", node->name.c_str(), "derived");
    data_row("kind", "enum", model::kind_name(node->kind), "derived");
    data_row("status", "enum", model::status_name(node->status), "derived");
    data_i32("child_count", "u32", static_cast<std::int32_t>(node->children.size()), "derived");
    for (const model::Property& property : node->properties) {
        if (property.visible) {
            data_property(property);
        }
    }

    if (node->runtimeEntity.has_value()) {
        data_u64("runtime_entity", "u64", *node->runtimeEntity, 16, "runtime");
    }
    if (node->objectSystemType.has_value()) {
        data_u64("object_system_type", "u8", *node->objectSystemType, 2, "runtime");
    }
    if (node->observationId.has_value()) {
        data_u64("observation_slot", "u64", *node->observationId, 16, "runtime");
    }
    if (node->triggerSourceHash.has_value()) {
        data_u64("trigger_source", "hash32", *node->triggerSourceHash, 8, "runtime");
    }
    if (node->triggerOverlapCount.has_value()) {
        data_i32("trigger_overlap_count",
                 "u32",
                 static_cast<std::int32_t>(*node->triggerOverlapCount),
                 "runtime");
    }
    if (node->triggerSelector.has_value()) {
        data_i32("trigger_selector", "i32", *node->triggerSelector, "runtime");
    }
    if (node->triggerEnabled.has_value()) {
        data_row("trigger_enabled", "bool", *node->triggerEnabled ? "true" : "false", "runtime");
    }
    if (node->triggerActive.has_value()) {
        data_row("trigger_active", "bool", *node->triggerActive ? "true" : "false", "runtime");
    }
    if (node->worldId.has_value()) {
        data_u64("world_id", "u64", *node->worldId, 16, "runtime");
    }
    if (node->tag.has_value()) {
        data_u64("tag_hash", "tag32", *node->tag, 8, "catalog");
    }
    if (node->classHash.has_value()) {
        data_u64("class_hash", "hash32", *node->classHash, 8, "catalog");
    }
    if (node->nameHash.has_value()) {
        data_u64("name_hash", "fnv32", *node->nameHash, 8, "catalog");
    }

    if (node->transform.has_value()) {
        const char* const transformOrigin = node->transformRuntime ? "runtime" : "catalog";
        data_vec3("position", node->transform->position, transformOrigin);
        if (node->transform->hasRotation) {
            data_vec3("rotation", node->transform->rotation, transformOrigin);
        }
        if (node->transform->hasScale) {
            data_vec3("scale", node->transform->scale, transformOrigin);
        }
    }
    if (node->linearVelocity.has_value()) {
        data_vec3("linear_velocity", *node->linearVelocity, "runtime");
    }
    if (node->bounds.has_value() && model::bounds_valid(*node->bounds)) {
        const char* const boundsOrigin = node->boundsProvenance.has_value()
                                             ? model::provenance_name(*node->boundsProvenance)
                                             : "unknown";
        data_vec3("bounds_min", node->bounds->minimum, boundsOrigin);
        data_vec3("bounds_max", node->bounds->maximum, boundsOrigin);
        data_row("bounds_provenance", "enum", boundsOrigin, boundsOrigin);
    } else if (node->kind == model::NodeKind::trigger && node->transform.has_value()) {
        data_row("trigger_shape", "state", "center observation only", "runtime");
    }

    const model::Source& source = node->source;
    if (!source.packageName.empty()) {
        data_row("package", "string", source.packageName.c_str(), "catalog");
    }
    if (!source.mapStem.empty()) {
        data_row("map_stem", "string", source.mapStem.c_str(), "catalog");
    }
    if (source.scenarioTag.has_value()) {
        data_u64("scenario_tag", "tag32", *source.scenarioTag, 8, "catalog");
    }
    if (source.spawnSetHash.has_value()) {
        data_u64("spawn_set_hash", "fnv32", *source.spawnSetHash, 8, "catalog");
    }
    if (source.activitySession.has_value()) {
        data_u64("activity_session", "u64", *source.activitySession, 16, "runtime");
    }
    if (source.activityIndex.has_value()) {
        data_i32("activity_index", "i32", *source.activityIndex, "runtime");
    }
    if (source.bubble.has_value()) {
        data_i32("bubble", "u16", *source.bubble, "runtime");
    }
    if (source.authoredPreview) {
        data_row("authored_preview", "bool", "true", "catalog");
    }

    if (node->activityLogicMetadata.has_value()) {
        const model::ActivityLogicMetadata& metadata = *node->activityLogicMetadata;
        data_u64("activity_logic_scenario", "tag32", metadata.scenarioTag, 8, "catalog");
        data_u64("activity_logic_definition", "tag32", metadata.definitionTag, 8, "catalog");
        data_row("activity_logic_role", "string", metadata.roleName.c_str(), "catalog");
        data_row("activity_logic_confidence", "string", metadata.confidenceName.c_str(), "catalog");
        data_i32("activity_logic_placement_count",
                 "u32",
                 static_cast<std::int32_t>(metadata.placementCount),
                 "catalog");
        std::size_t relationshipIndex = 0;
        for (const model::ActivityLogicRelationship& relationship : metadata.relationships) {
            std::array<char, 64> field{};
            std::array<char, 128> value{};
            std::snprintf(
                field.data(), field.size(), "activity_logic_relationship_%zu", relationshipIndex++);
            std::snprintf(value.data(),
                          value.size(),
                          "%s definition=0x%08X name=0x%08X count=%u",
                          relationship.outgoing ? "outgoing" : "incoming",
                          relationship.definitionTag,
                          relationship.nameHash,
                          relationship.occurrenceCount);
            data_row(field.data(), "relationship", value.data(), "catalog");
        }
    }

    ImGui::EndTable();
    ImGui::TextDisabled(
        "Raw package offsets and encoded sizes are unavailable in the current reduced catalog.");
}

void draw_diagnostics() noexcept {
    const viewport::Result& overlay = g_state.lastViewportResult;
    const ui_memory::Stats uiMemory = ui_memory::snapshot();
    ImGui::TextUnformatted("Viewport overlay");
    ImGui::TextDisabled("eligible %zu  projected %zu  off-screen %zu  stale %zu",
                        overlay.eligibleNodes,
                        overlay.projectedNodes,
                        overlay.offscreenNodes,
                        overlay.staleNodes);
    ImGui::TextDisabled("labels attempted %zu  placed %zu  collision-omitted %zu",
                        overlay.attemptedLabels,
                        overlay.placedLabels,
                        overlay.collisionOmittedLabels);
    ImGui::TextDisabled(
        "draw plan %zu vertices  %zu indices", overlay.plannedVertices, overlay.plannedIndices);
    ImGui::TextDisabled("prepare %.3f ms  label layout %.3f ms",
                        overlay.preparationMilliseconds,
                        overlay.labelLayoutMilliseconds);
    if (overlay.allocationFailure) {
        ImGui::TextColored(kFailure,
                           overlay.reservationFailure
                               ? "Overlay draw reservation failed; this frame was skipped."
                               : "Overlay preparation ran out of memory; this frame was skipped.");
    }
    ImGui::TextDisabled("UI arena %zu / %zu bytes live  largest free %zu  misses %zu",
                        uiMemory.outstandingBytes,
                        uiMemory.capacityBytes,
                        uiMemory.largestFreeBytes,
                        uiMemory.arenaMisses);
    ImGui::TextDisabled("UI spill %zu allocations  %zu bytes live  %zu high-water",
                        uiMemory.spillOutstandingAllocations,
                        uiMemory.spillOutstandingBytes,
                        uiMemory.spillHighWaterBytes);
    if (uiMemory.allocationFailures != 0) {
        ImGui::TextColored(kFailure,
                           "UI allocation failures %zu; last request %zu bytes",
                           uiMemory.allocationFailures,
                           uiMemory.lastFailedBytes);
    }
    const content::statics::Progress footprintProgress = content::statics::progress();
    if (footprintProgress.allocationFailure) {
        ImGui::TextColored(kFailure,
                           "Static-footprint worker ran out of memory; its request was aborted.");
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Producer readiness");
    if (ImGui::BeginTable(
            "##inspector_producers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Producer");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Sequence");
        ImGui::TableSetupColumn("Copied");
        ImGui::TableSetupColumn("Failure");
        ImGui::TableHeadersRow();
        for (const model::ProviderReport& report : world().providerReports) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(model::producer_name(report.producer));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(!report.installed ? "not installed"
                                   : report.ready    ? "ready"
                                                     : "not ready");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%llu", static_cast<unsigned long long>(report.sequence));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu / %llu%s",
                        static_cast<unsigned long long>(report.copiedCount),
                        static_cast<unsigned long long>(report.declaredCount),
                        report.truncated ? " bounded" : "");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextWrapped("%s", report.failure.empty() ? "-" : report.failure.c_str());
        }
        ImGui::EndTable();
    }
    const debug_scene::DepthStatus depth = debug_scene::status();
    const model::SceneFramePtr scene = debug_scene::frame();
    const model::OverlayStats sceneStats = scene ? scene->stats : model::OverlayStats{};
    ImGui::TextDisabled("Helper backend: %s | frame: %llu | lines: %zu | glyphs: %zu%s",
                        model::helper_backend_name(depth.backend),
                        static_cast<unsigned long long>(depth.capturedFrame),
                        depth.submittedLines,
                        depth.submittedGlyphs,
                        sceneStats.truncated ? " truncated" : "");
    ImGui::TextDisabled(
        "Presentation: %zu shown | density %zu | distance %zu | offscreen %zu | budget %zu",
        sceneStats.shownNodes,
        sceneStats.densityFilteredNodes,
        sceneStats.distanceFilteredNodes,
        sceneStats.viewFilteredNodes,
        sceneStats.omittedNodes + sceneStats.partialNodes);
    ImGui::TextDisabled("Mode: %s | focus context: %zu",
                        model::overlay_detail_name(g_state.overlayDetail),
                        sceneStats.focusContextNodes);
    ImGui::TextDisabled("Failure: %s | picking: %s",
                        model::helper_failure_name(depth.failureReason),
                        model::picking_readiness_name(depth.picking));
    const model::PickRequest pickRequest = model::pick_request();
    const model::PickResult pickResult = model::pick_result();
    ImGui::TextDisabled("Pick request: %llu @ %u,%u | result: %llu%s | node: 0x%llX | graph: %u",
                        static_cast<unsigned long long>(pickRequest.sequence),
                        pickRequest.x,
                        pickRequest.y,
                        static_cast<unsigned long long>(pickResult.requestSequence),
                        pickResult.ready ? " ready" : " pending",
                        static_cast<unsigned long long>(pickResult.node.value),
                        pickResult.graphGeneration);
    const renderer::native_debug::ObserverStatus nativeObserver =
        renderer::native_debug::observer_status();
    ImGui::TextDisabled("Native observer: %s | signatures: %s | failure: %s",
                        nativeObserver.installed ? "installed" : "inactive",
                        nativeObserver.signaturesValid ? "valid" : "unproven",
                        renderer::native_debug::observer_failure_name(nativeObserver.failure));
    ImGui::TextDisabled("Calls: %llu | accepted: %llu | rejected: %llu | concurrent: %llu",
                        static_cast<unsigned long long>(nativeObserver.calls),
                        static_cast<unsigned long long>(nativeObserver.acceptedSamples),
                        static_cast<unsigned long long>(nativeObserver.rejectedSamples),
                        static_cast<unsigned long long>(nativeObserver.concurrentSamples));
    if (nativeObserver.observed) {
        ImGui::TextDisabled("Last view: %u/%u | frame %llu | thread %u (%u workers) | flags 0x%X | "
                            "framebuffer %ux%u | shapes %u",
                            nativeObserver.viewIndex,
                            nativeObserver.viewCount,
                            static_cast<unsigned long long>(nativeObserver.engineFrame),
                            nativeObserver.threadId,
                            nativeObserver.observedThreadCount,
                            nativeObserver.passFlags,
                            static_cast<unsigned>(nativeObserver.framebufferWidth),
                            static_cast<unsigned>(nativeObserver.framebufferHeight),
                            nativeObserver.viewShapeCount);
        ImGui::TextDisabled("Primary matrix pair: %llu equal / %llu different / %llu paired",
                            static_cast<unsigned long long>(nativeObserver.equalMatrixFrames),
                            static_cast<unsigned long long>(nativeObserver.differentMatrixFrames),
                            static_cast<unsigned long long>(nativeObserver.pairedFrames));
        ImGui::TextDisabled(
            "Viewport: %.1f, %.1f, %.1f x %.1f | normalized %.4f, %.4f, %.4f, %.4f%s",
            static_cast<double>(nativeObserver.viewport.x),
            static_cast<double>(nativeObserver.viewport.y),
            static_cast<double>(nativeObserver.viewport.width),
            static_cast<double>(nativeObserver.viewport.height),
            static_cast<double>(nativeObserver.normalizedViewport[0]),
            static_cast<double>(nativeObserver.normalizedViewport[1]),
            static_cast<double>(nativeObserver.normalizedViewport[2]),
            static_cast<double>(nativeObserver.normalizedViewport[3]),
            nativeObserver.fullViewport ? " full" : "");
    }
    const renderer::depth_observer::Status depthObserver = renderer::depth_observer::status();
    ImGui::TextDisabled("Depth observer: %s | proof: %s | release: %s",
                        depthObserver.hooksResolved ? "active" : "inactive",
                        depthObserver.eligible ? "eligible" : "collecting",
                        depthObserver.captureEnabled ? "capture" : "passive");
    ImGui::TextDisabled(
        "Depth class: fmt %u | %ux%u | %ux MSAA | identities %u | classes %u (%u eligible)",
        static_cast<unsigned>(depthObserver.descriptor.format),
        depthObserver.descriptor.width,
        depthObserver.descriptor.height,
        depthObserver.descriptor.sampleCount,
        depthObserver.resourceIdentityCount,
        depthObserver.descriptorClassCount,
        depthObserver.eligibleClassCount);
    ImGui::TextDisabled("Proof window: %u/%u final | matrix hashes %u current / %u peak / %u "
                        "required | %s | failure: %s",
                        depthObserver.qualifyingFrames,
                        depthObserver.observedFrames,
                        depthObserver.currentDistinctMatrixHashes,
                        depthObserver.distinctMatrixHashes,
                        renderer::depth_observer::kRequiredMatrixHashes,
                        renderer::depth_observer::convention_name(depthObserver.convention),
                        renderer::depth_observer::failure_name(depthObserver.failure));
    ImGui::TextDisabled(
        "Depth convention evidence: clear %.4f | comparison %u | clear %s | comparisons %s",
        static_cast<double>(depthObserver.clearDepth),
        static_cast<unsigned>(depthObserver.comparison),
        depthObserver.clearConsistent ? "consistent" : "mixed",
        depthObserver.comparisonConsistent ? "consistent" : "mixed");
    ImGui::TextDisabled("Bindings: %llu | states: %llu | clears: %llu | dropped: %llu",
                        static_cast<unsigned long long>(depthObserver.renderTargetBindings),
                        static_cast<unsigned long long>(depthObserver.depthStateChanges),
                        static_cast<unsigned long long>(depthObserver.depthClears),
                        static_cast<unsigned long long>(depthObserver.droppedObservations));
    ImGui::Separator();
    ImGui::TextUnformatted("Runtime structure probes");
    ImGui::SameLine();
    if (control_button("Probe static structure", {0.0F, control_height()}, false, true)) {
        content::statics::request_structure_probe();
    }
    ImGui::SameLine();
    if (content::statics::structure_probe_running()) {
        ImGui::TextDisabled("probing - see middleware log");
    } else if (content::statics::structure_probe_finished()) {
        ImGui::TextDisabled("finished - see middleware log");
    } else {
        ImGui::TextDisabled("not run");
    }
    ImGui::Separator();

    const auto& diagnostics = world().diagnostics;
    if (diagnostics.empty()) {
        ImGui::TextDisabled("No inspector diagnostics for this world snapshot.");
        return;
    }

    ImGui::TextDisabled("%zu diagnostics", diagnostics.size());
    ImGui::SameLine();
    if (control_button("Copy diagnostics", {0.0F, control_height()}, false, true)) {
        std::string report;
        for (const model::Diagnostic& diagnostic : diagnostics) {
            const char* prefix = diagnostic.severity == model::Diagnostic::Severity::error ? "ERROR"
                                 : diagnostic.severity == model::Diagnostic::Severity::warning
                                     ? "WARN"
                                     : "INFO";
            report.append(prefix);
            report.append(": ");
            report.append(diagnostic.message);
            report.push_back('\n');
        }
        copy_text(report);
    }
    ImGui::Separator();

    for (const model::Diagnostic& diagnostic : diagnostics) {
        ImVec4 color = kMuted;
        const char* prefix = "INFO";
        if (diagnostic.severity == model::Diagnostic::Severity::warning) {
            color = kWarning;
            prefix = "WARN";
        } else if (diagnostic.severity == model::Diagnostic::Severity::error) {
            color = kFailure;
            prefix = "ERROR";
        }
        ImGui::TextColored(color, "%s", prefix);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", diagnostic.message.c_str());
    }
}

void select_event_node(std::string_view identity) noexcept {
    for (const model::Node& node : world().graph.nodes()) {
        if (capture::stable_identity(world(), node) == identity) {
            select_node(node.id);
            return;
        }
    }
}

void draw_export_status() noexcept {
    const capture::ExportResult& lastExport = g_state.session.last_export();
    if (lastExport.success) {
        ImGui::TextWrapped("Exported: %ls", lastExport.path.data());
    } else if (lastExport.error[0] != '\0') {
        ImGui::TextColored(kFailure, "Export failed: %s", lastExport.error.data());
    }
}

void draw_event_rows(std::span<const capture::ChangeEvent> events) noexcept {
    const std::string_view filter(g_state.eventFilter.data());
    for (const capture::ChangeEvent& event : events) {
        if (!filter.empty() && event.identity.find(filter) == std::string::npos
            && event.nodeName.find(filter) == std::string::npos
            && event.nodeKind.find(filter) == std::string::npos
            && event.field.find(filter) == std::string::npos
            && event.before.find(filter) == std::string::npos
            && event.after.find(filter) == std::string::npos) {
            continue;
        }
        std::array<char, 768> label{};
        const int written = std::snprintf(label.data(),
                                          label.size(),
                                          "#%llu %s  %s [%s]  %s  %s -> %s",
                                          static_cast<unsigned long long>(event.sequence),
                                          capture::change_kind_name(event.kind),
                                          event.nodeName.c_str(),
                                          event.nodeKind.c_str(),
                                          event.field.c_str(),
                                          event.before.c_str(),
                                          event.after.c_str());
        if (written > 0 && ImGui::Selectable(label.data(), false)) {
            select_event_node(event.identity);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s\nprovenance: %s", event.identity.c_str(), event.provenance.c_str());
        }
    }
}

void draw_events() noexcept {
    capture::History& history = g_state.session.history();
    const bool recording = history.recording();
    if (control_button(recording ? "Pause tracking" : "Start tracking",
                       {0.0F, control_height()},
                       recording,
                       true)) {
        history.set_recording(!recording, world());
    }
    ImGui::SameLine();
    if (control_button("Clear", {0.0F, control_height()}, false, true)) {
        history.clear();
    }
    ImGui::SameLine();
    if (control_button("Export events", {0.0F, control_height()}, false, true)) {
        g_state.session.export_events();
    }
    ImGui::SameLine();
    if (control_button("Snapshot JSON", {0.0F, control_height()}, false, true)) {
        g_state.session.export_json();
    }
    ImGui::SameLine();
    if (control_button("Node CSV", {0.0F, control_height()}, false, true)) {
        g_state.session.export_csv();
    }
    bool optionsChanged = false;
    optionsChanged = ImGui::Checkbox("Runtime only", &g_state.trackRuntimeOnly) || optionsChanged;
    ImGui::SameLine();
    optionsChanged =
        ImGui::Checkbox("Track transforms", &g_state.trackTransforms) || optionsChanged;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Transform changes use a 5 cm position threshold to suppress frame noise.");
    }
    if (optionsChanged) {
        history.set_options(capture::ChangeTrackingOptions{g_state.trackRuntimeOnly,
                                                           g_state.trackTransforms,
                                                           0.05F},
                            world());
    }
    ImGui::SetNextItemWidth((std::max)(180.0F, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##event_filter",
                             "Filter node, kind, identity, field, or value",
                             g_state.eventFilter.data(),
                             g_state.eventFilter.size());
    draw_export_status();
    ImGui::Separator();
    ImGui::TextDisabled("%zu / %zu events", history.events().size(), capture::kEventCapacity);
    draw_event_rows(history.events());
}

void draw_compare() noexcept {
    if (control_button("Set baseline", {0.0F, control_height()}, false, true)) {
        g_state.session.capture_comparison_baseline();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_state.session.comparison_baseline().has_value());
    if (control_button("Compare now", {0.0F, control_height()}, false, true)) {
        g_state.session.compare_with_baseline();
    }
    ImGui::EndDisabled();
    if (!g_state.session.comparison_baseline().has_value()) {
        ImGui::TextDisabled("Capture a baseline to compare complete pointer-free snapshots.");
        return;
    }
    ImGui::TextDisabled(
        "Baseline tick: %llu, changes: %zu",
        static_cast<unsigned long long>(g_state.session.comparison_baseline()->capturedTick),
        g_state.session.comparison_events().size());
    draw_event_rows(g_state.session.comparison_events());
}

void draw_bottom_dock() noexcept {
    const float contentWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const std::span panels{kBottomPanels};
    const char* hide = "Hide";
    const float hideWidth = ImGui::CalcTextSize(hide).x + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float headerGap = scaled(8.0F);
    const float minimumTabWidth = scaled(76.0F);
    float widestLabel = 0.0F;
    for (const BottomPanelDescriptor& panel : panels) {
        widestLabel = (std::max)(widestLabel, ImGui::CalcTextSize(panel.label).x);
    }
    const float preferredTabWidth =
        (std::max)(minimumTabWidth,
                   widestLabel + ImGui::GetStyle().FramePadding.x * 2.0F + scaled(8.0F));
    const float tabSpacing = spacing * static_cast<float>(panels.size() - 1U);
    const float preferredHeaderWidth =
        preferredTabWidth * static_cast<float>(panels.size()) + tabSpacing + headerGap + hideWidth;
    const bool compactHeader = contentWidth < preferredHeaderWidth;
    const float tabArea = compactHeader
                              ? contentWidth
                              : preferredTabWidth * static_cast<float>(panels.size()) + tabSpacing;
    const float tabWidth =
        (std::max)(1.0F, (tabArea - tabSpacing) / static_cast<float>(panels.size()));

    const auto tab = [tabWidth](const char* label, BottomTab tabValue) noexcept {
        if (tab_button(label, g_state.bottomTab == tabValue, tabWidth)) {
            g_state.bottomTab = tabValue;
        }
    };

    for (std::size_t index = 0; index < panels.size(); ++index) {
        if (index != 0) {
            ImGui::SameLine();
        }
        tab(panels[index].label, panels[index].tab);
    }

    if (compactHeader) {
        if (control_button("Hide bottom panel",
                           {ImGui::GetContentRegionAvail().x, control_height()},
                           false,
                           true)) {
            g_state.bottomCollapsed = true;
            persist_layout();
        }
    } else {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - hideWidth);
        if (control_button(hide, {hideWidth, control_height()}, false, true)) {
            g_state.bottomCollapsed = true;
            persist_layout();
        }
    }

    ImGui::Separator();
    const float contentHeight = (std::max)(0.0F, ImGui::GetContentRegionAvail().y);
    const ImGuiWindowFlags contentFlags =
        g_state.bottomTab == BottomTab::data ? ImGuiWindowFlags_NoScrollbar : ImGuiWindowFlags_None;
    ImGui::BeginChild("##bottom_content", {0.0F, contentHeight}, false, contentFlags);
    switch (g_state.bottomTab) {
    case BottomTab::references:
        draw_references();
        break;
    case BottomTab::data:
        draw_data();
        break;
    case BottomTab::events:
        draw_events();
        break;
    case BottomTab::compare:
        draw_compare();
        break;
    case BottomTab::diagnostics:
        draw_diagnostics();
        break;
    }
    ImGui::EndChild();
}

void draw_node_context_menu() noexcept {
    if (!ImGui::BeginPopup("##world_inspector_context")) {
        return;
    }
    const model::Node* node = world().graph.node(g_state.contextTarget);
    if (node == nullptr) {
        ImGui::EndPopup();
        return;
    }

    const camera::Status status = camera::status();
    const CameraTeleportAvailability cameraTeleport =
        camera_teleport_availability(node->id, status);
    ImGui::BeginDisabled(!cameraTeleport.available());
    if (ImGui::MenuItem("Focus camera", "F")) {
        focus(node->id);
    }
    ImGui::EndDisabled();
    if (!cameraTeleport.available() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Unavailable: %s", cameraTeleport.unavailableReason);
    }
    if (model::supports(node->actions, model::Action::hide)
        && ImGui::MenuItem(hidden(node->id) ? "Show" : "Hide", "H")) {
        toggle_hidden(node->id);
    }
    if (model::supports(node->actions, model::Action::isolate)
        && ImGui::MenuItem("Isolate", "Shift+H")) {
        isolate(node->id);
    }
    if (!g_state.hidden.empty() && ImGui::MenuItem("Show All", "Alt+H")) {
        show_all();
    }

    ImGui::Separator();
    if (model::supports(node->actions, model::Action::copyId)
        && ImGui::MenuItem("Copy ID", "Ctrl+C")) {
        copy_id(node->id);
    }
    if (node->tag.has_value() && model::supports(node->actions, model::Action::copyTag)
        && ImGui::MenuItem("Copy Tag Hash")) {
        copy_tag(*node->tag);
    }
    if (node->transform.has_value() && model::supports(node->actions, model::Action::copyPosition)
        && ImGui::MenuItem("Copy Position", "Ctrl+Shift+C")) {
        copy_position(*node->transform);
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!node->parent);
    if (ImGui::MenuItem("Select Parent")) {
        select_node(node->parent);
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void draw_helper_controls() noexcept {
    static bool persistPending = false;
    bool changed = false;
    if (control_button("Helpers", {0.0F, control_height()}, false, true)) {
        ImGui::OpenPopup("##viewport_helper_controls");
    }
    ImGui::SameLine();
    if (g_state.overlayDetail == viewport::Detail::all) {
        ImGui::TextDisabled("All admitted · hard cap 1024 · %.0f px",
                            static_cast<double>(g_state.glyphSizePixels));
    } else {
        ImGui::TextDisabled("%s · %u node cap · %.0f px",
                            model::overlay_detail_name(g_state.overlayDetail),
                            g_state.maximumVisibleNodes,
                            static_cast<double>(g_state.glyphSizePixels));
    }

    const bool popupOpen = ImGui::BeginPopup("##viewport_helper_controls");
    if (popupOpen) {
        ImGui::TextUnformatted("Helpers");
        ImGui::Separator();

        static constexpr const char* detailNames[]{
            "Selected only",
            "Selected and nearby",
            "All admitted",
            "Adaptive",
            "Camera nearby",
        };
        int detail = static_cast<int>(g_state.overlayDetail);
        ImGui::SetNextItemWidth(scaled(220.0F));
        if (ImGui::Combo("Detail mode", &detail, detailNames, std::size(detailNames))) {
            g_state.overlayDetail = static_cast<viewport::Detail>(detail);
            changed = true;
        }

        int maximumVisible = static_cast<int>(g_state.maximumVisibleNodes);
        ImGui::SetNextItemWidth(scaled(220.0F));
        ImGui::BeginDisabled(g_state.overlayDetail == viewport::Detail::all);
        if (ImGui::SliderInt("Maximum visible nodes", &maximumVisible, 32, 1024)) {
            g_state.maximumVisibleNodes = static_cast<std::uint32_t>(maximumVisible);
            changed = true;
        }
        ImGui::EndDisabled();
        if (g_state.overlayDetail == viewport::Detail::all
            && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("All admitted uses the fixed 1,024-node safety ceiling.");
        }
        ImGui::SetNextItemWidth(scaled(220.0F));
        changed |=
            ImGui::SliderFloat("Nearby radius", &g_state.nearbyRadius, 5.0F, 250.0F, "%.0f m");
        ImGui::SetNextItemWidth(scaled(220.0F));
        changed |= ImGui::SliderFloat("Marker size",
                                      &g_state.glyphSizePixels,
                                      inspection::settings::kMinimumGlyphSizePixels,
                                      inspection::settings::kMaximumGlyphSizePixels,
                                      "%.0f px");
        ImGui::SetNextItemWidth(scaled(220.0F));
        changed |=
            ImGui::SliderFloat("Line width", &g_state.lineWidthPixels, 1.0F, 4.0F, "%.1f px");
        float baseOpacityPercent = g_state.baseOpacity * 100.0F;
        ImGui::SetNextItemWidth(scaled(220.0F));
        if (ImGui::SliderFloat("Base opacity", &baseOpacityPercent, 15.0F, 100.0F, "%.0f%%")) {
            g_state.baseOpacity = baseOpacityPercent * 0.01F;
            changed = true;
        }
        float focusOpacityPercent = g_state.focusContextOpacity * 100.0F;
        ImGui::SetNextItemWidth(scaled(220.0F));
        if (ImGui::SliderFloat(
                "Focus-context opacity", &focusOpacityPercent, 5.0F, 100.0F, "%.0f%%")) {
            g_state.focusContextOpacity = focusOpacityPercent * 0.01F;
            changed = true;
        }

        ImGui::Separator();
        ImGui::TextDisabled("Role legend");
        static constexpr std::array<inspection::HelperGlyph, 18> glyphs{
            inspection::HelperGlyph::spawn,
            inspection::HelperGlyph::runtimeEntity,
            inspection::HelperGlyph::spawnRule,
            inspection::HelperGlyph::squad,
            inspection::HelperGlyph::trigger,
            inspection::HelperGlyph::spatial,
            inspection::HelperGlyph::objective,
            inspection::HelperGlyph::device,
            inspection::HelperGlyph::interactable,
            inspection::HelperGlyph::action,
            inspection::HelperGlyph::target,
            inspection::HelperGlyph::condition,
            inspection::HelperGlyph::competitive,
            inspection::HelperGlyph::audio,
            inspection::HelperGlyph::physics,
            inspection::HelperGlyph::geometry,
            inspection::HelperGlyph::unknown,
        };
        const auto markerColor = [](inspection::HelperGlyph glyph) noexcept {
            const std::array<float, 4> color = inspection::helper_role_color(glyph);
            const auto lane = [](float value) noexcept {
                return static_cast<int>((std::clamp)(value, 0.0F, 1.0F) * 255.0F + 0.5F);
            };
            return IM_COL32(lane(color[0]), lane(color[1]), lane(color[2]), 255);
        };
        if (ImGui::BeginTable("##helper_role_legend",
                              4,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            for (std::size_t index = 0; index < glyphs.size(); index += 2U) {
                ImGui::TableNextRow();
                for (std::size_t pair = 0; pair < 2U; ++pair) {
                    const std::size_t glyphIndex = index + pair;
                    if (glyphIndex >= glyphs.size()) {
                        break;
                    }
                    const inspection::HelperGlyph glyph = glyphs[glyphIndex];
                    ImGui::TableSetColumnIndex(static_cast<int>(pair * 2U));
                    const ImVec2 minimum = ImGui::GetCursorScreenPos();
                    const float diameter = scaled(12.0F);
                    const float radius = diameter * 0.5F;
                    const ImVec2 center{minimum.x + radius, minimum.y + radius};
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    if (inspection::helper_marker_shape(glyph)
                        == inspection::HelperMarkerShape::circle) {
                        drawList->AddCircleFilled(center, radius, IM_COL32(6, 9, 14, 255), 16);
                        drawList->AddCircleFilled(center,
                                                  (std::max)(scaled(1.0F), radius - scaled(1.5F)),
                                                  markerColor(glyph),
                                                  16);
                    } else {
                        drawList->AddRectFilled(minimum,
                                                {minimum.x + diameter, minimum.y + diameter},
                                                IM_COL32(6, 9, 14, 255));
                        const float inset = scaled(1.5F);
                        drawList->AddRectFilled(
                            {minimum.x + inset, minimum.y + inset},
                            {minimum.x + diameter - inset, minimum.y + diameter - inset},
                            markerColor(glyph));
                    }
                    ImGui::Dummy({diameter, diameter});
                    ImGui::TableSetColumnIndex(static_cast<int>(pair * 2U + 1U));
                    ImGui::TextUnformatted(inspection::helper_glyph_name(glyph));
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (ImGui::Button("Reset defaults")) {
            g_state.overlayDetail = viewport::Detail::adaptive;
            g_state.maximumVisibleNodes = inspection::settings::kDefaultMaximumVisibleNodes;
            g_state.nearbyRadius = inspection::settings::kDefaultNearbyRadius;
            g_state.glyphSizePixels = inspection::settings::kDefaultGlyphSizePixels;
            g_state.lineWidthPixels = inspection::settings::kDefaultLineWidthPixels;
            g_state.baseOpacity = inspection::settings::kDefaultBaseOpacity;
            g_state.focusContextOpacity = inspection::settings::kDefaultFocusContextOpacity;
            changed = true;
        }
        ImGui::EndPopup();
    }
    if (changed) {
        persistPending = true;
    }
    if (!popupOpen && persistPending) {
        persist_layout();
        persistPending = false;
    }
}

void handle_shortcuts() noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }

    const model::NodeId selected = g_state.selection.selected();
    const model::Node* node = world().graph.node(selected);

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
        g_state.focusSearch = true;
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && node != nullptr) {
        if (io.KeyShift && node->transform.has_value()
            && model::supports(node->actions, model::Action::copyPosition)) {
            copy_position(*node->transform);
        } else if (model::supports(node->actions, model::Action::copyId)) {
            copy_id(node->id);
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        clear_selection();
    }
    if (selected && ImGui::IsKeyPressed(ImGuiKey_F)) {
        focus(selected);
    }
    if (!ImGui::IsKeyPressed(ImGuiKey_H)) {
        return;
    }
    if (io.KeyAlt) {
        show_all();
    } else if (io.KeyShift) {
        isolate(selected);
    } else {
        toggle_hidden(selected);
    }
}

viewport::Result draw_activity_map() noexcept {
    viewport::Result result{};

    static std::vector<std::uint32_t> graphs;
    graphs.clear();
    for (const model::Node& node : world().graph.nodes()) {
        if (node.producer == model::Producer::activityCatalog
            && node.kind == model::NodeKind::activityGraph && node.activityMetadata.has_value()
            && node.activityMetadata->graphHash != 0) {
            graphs.push_back(node.activityMetadata->graphHash);
        }
    }
    std::ranges::sort(graphs);
    graphs.erase(std::unique(graphs.begin(), graphs.end()), graphs.end());

    if (graphs.empty()) {
        ImGui::TextDisabled("Activity catalog is not loaded or contains no graph records.");
        return result;
    }
    if (std::ranges::find(graphs, g_state.selectedActivityGraphHash) == graphs.end()) {
        g_state.selectedActivityGraphHash = graphs.front();
    }
    auto current = std::ranges::find(graphs, g_state.selectedActivityGraphHash);
    std::size_t graphIndex = static_cast<std::size_t>(current - graphs.begin());

    ImGui::BeginDisabled(graphIndex == 0);
    if (control_button("Previous graph", {0.0F, control_height()}, false, graphIndex != 0)) {
        open_activity_graph(graphs[graphIndex - 1]);
        --graphIndex;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::array<char, 80> preview{};
    std::snprintf(preview.data(),
                  preview.size(),
                  "Graph 0x%08X (%zu/%zu)",
                  g_state.selectedActivityGraphHash,
                  graphIndex + 1,
                  graphs.size());
    ImGui::SetNextItemWidth(scaled(220.0F));
    if (ImGui::BeginCombo("##activity_graph_selector", preview.data())) {
        for (std::size_t index = 0; index < graphs.size(); ++index) {
            std::array<char, 48> label{};
            std::snprintf(label.data(), label.size(), "0x%08X", graphs[index]);
            if (ImGui::Selectable(label.data(), index == graphIndex)) {
                open_activity_graph(graphs[index]);
                graphIndex = index;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const bool hasNext = graphIndex + 1 < graphs.size();
    ImGui::BeginDisabled(!hasNext);
    if (control_button("Next graph", {0.0F, control_height()}, false, hasNext)) {
        open_activity_graph(graphs[graphIndex + 1]);
        ++graphIndex;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("collector %llu / build %llu / target %u",
                        static_cast<unsigned long long>(
                            root_u64("activity_catalog_collector_version")),
                        static_cast<unsigned long long>(root_u64("activity_catalog_build")),
                        activity_catalog::kTargetContentBuild);
    ImGui::TextDisabled("Authored positions only; each canvas is one graph coordinate space. No "
                        "node connections are present in this catalog.");
    ImGui::Separator();

    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 1.0F);
    size.y = (std::max)(size.y, 1.0F);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
    ImGui::InvisibleButton("##world_activity_map",
                           size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hoveredCanvas = ImGui::IsItemHovered();
    const ImVec2 pointer = ImGui::GetIO().MousePos;

    static std::vector<const model::Node*> nodes;
    nodes.clear();
    for (const model::Node& node : world().graph.nodes()) {
        if (node.producer == model::Producer::activityCatalog
            && node.kind == model::NodeKind::activityGraphNode && node.activityMetadata.has_value()
            && node.activityMetadata->graphHash == g_state.selectedActivityGraphHash) {
            nodes.push_back(&node);
        }
    }
    std::ranges::stable_sort(nodes, [](const model::Node* left, const model::Node* right) {
        return left->activityMetadata->nodeHash < right->activityMetadata->nodeHash;
    });

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(minimum, maximum, IM_COL32(11, 12, 17, 255));
    drawList->PushClipRect(minimum, maximum, true);
    if (nodes.empty()) {
        drawList->AddText({minimum.x + scaled(12.0F), minimum.y + scaled(12.0F)},
                          ImGui::GetColorU32(kMuted),
                          "This activity graph has no authored nodes.");
        drawList->PopClipRect();
        return result;
    }

    float minimumX = (std::numeric_limits<float>::max)();
    float minimumY = (std::numeric_limits<float>::max)();
    float maximumX = (std::numeric_limits<float>::lowest)();
    float maximumY = (std::numeric_limits<float>::lowest)();
    for (const model::Node* node : nodes) {
        minimumX = (std::min)(minimumX, node->activityMetadata->authoredPosition[0]);
        minimumY = (std::min)(minimumY, node->activityMetadata->authoredPosition[1]);
        maximumX = (std::max)(maximumX, node->activityMetadata->authoredPosition[0]);
        maximumY = (std::max)(maximumY, node->activityMetadata->authoredPosition[1]);
    }
    const float margin = scaled(28.0F);
    const float cardWidth = scaled(158.0F);
    const float cardHeight = scaled(48.0F);
    const float usableWidth = (std::max)(1.0F, size.x - margin * 2.0F - cardWidth);
    const float usableHeight = (std::max)(1.0F, size.y - margin * 2.0F - cardHeight);
    const float spanX = maximumX - minimumX;
    const float spanY = maximumY - minimumY;
    const auto screen = [&](const model::Node& node) {
        const float x =
            spanX > 0.0F ? (node.activityMetadata->authoredPosition[0] - minimumX) / spanX : 0.5F;
        const float y =
            spanY > 0.0F ? (node.activityMetadata->authoredPosition[1] - minimumY) / spanY : 0.5F;
        return ImVec2{minimum.x + margin + x * usableWidth + cardWidth * 0.5F,
                      minimum.y + margin + (1.0F - y) * usableHeight + cardHeight * 0.5F};
    };

    struct ActivityCard final {
        const model::Node* node{};
        ImVec2 minimum{};
        ImVec2 maximum{};
    };
    static std::vector<ActivityCard> cards;
    cards.clear();
    cards.reserve(nodes.size());
    for (const model::Node* node : nodes) {
        const ImVec2 center = screen(*node);
        cards.push_back({node,
                         {center.x - cardWidth * 0.5F, center.y - cardHeight * 0.5F},
                         {center.x + cardWidth * 0.5F, center.y + cardHeight * 0.5F}});
    }

    static std::size_t coincidentCycle{};
    static std::uint32_t coincidentGraph{};
    static std::array<float, 2> coincidentPosition{};
    static std::vector<std::size_t> hits;
    hits.clear();
    if (hoveredCanvas) {
        for (std::size_t index = cards.size(); index-- > 0;) {
            const ActivityCard& card = cards[index];
            if (pointer.x >= card.minimum.x && pointer.x <= card.maximum.x
                && pointer.y >= card.minimum.y && pointer.y <= card.maximum.y) {
                hits.push_back(index);
            }
        }
    }
    if (!hits.empty()) {
        const auto position = cards[hits.front()].node->activityMetadata->authoredPosition;
        if (coincidentGraph != g_state.selectedActivityGraphHash
            || coincidentPosition != position) {
            coincidentGraph = g_state.selectedActivityGraphHash;
            coincidentPosition = position;
            coincidentCycle = 0;
        }
        coincidentCycle %= hits.size();
        result.hovered = cards[hits[coincidentCycle]].node->id;
    }

    for (const ActivityCard& card : cards) {
        const bool selected = card.node->id == g_state.selection.selected();
        const bool isHovered = card.node->id == result.hovered;
        drawList->AddRectFilled(
            card.minimum,
            card.maximum,
            selected ? IM_COL32(54, 48, 35, 250)
                     : (isHovered ? IM_COL32(42, 44, 56, 250) : IM_COL32(25, 27, 35, 245)),
            scaled(3.0F));
        drawList->AddRect(card.minimum,
                          card.maximum,
                          selected ? IM_COL32(228, 181, 79, 255) : IM_COL32(52, 55, 68, 255),
                          scaled(3.0F));
        drawList->AddText({card.minimum.x + scaled(8.0F), card.minimum.y + scaled(7.0F)},
                          IM_COL32(231, 231, 224, 255),
                          card.node->name.c_str());
        std::array<char, 64> detail{};
        std::snprintf(detail.data(),
                      detail.size(),
                      "%u activity refs",
                      card.node->activityMetadata->referenceCount);
        drawList->AddText({card.minimum.x + scaled(8.0F), card.minimum.y + scaled(27.0F)},
                          IM_COL32(145, 149, 158, 255),
                          detail.data());
    }
    drawList->PopClipRect();

    if (!hits.empty() && hits.size() > 1 && hoveredCanvas) {
        ImGui::BeginTooltip();
        ImGui::Text("%zu nodes share this authored position", hits.size());
        ImGui::TextDisabled("Repeated clicks cycle the coincident nodes without moving their "
                            "authored coordinates.");
        for (std::size_t index = 0; index < (std::min)(hits.size(), std::size_t{8}); ++index) {
            const model::Node* node = cards[hits[index]].node;
            ImGui::Text("%s%s", node->id == result.hovered ? "> " : "  ", node->name.c_str());
        }
        ImGui::EndTooltip();
    }

    if (hoveredCanvas && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (result.hovered) {
            result.selected = result.hovered;
            if (hits.size() > 1) {
                coincidentCycle = (coincidentCycle + 1U) % hits.size();
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
    return result;
}

[[nodiscard]] std::span<const worlds::Summary> activity_rows() noexcept {
    thread_local std::array<worlds::Summary, worlds::kWorldCapacity> rows{};
    std::size_t count = 0;
    std::size_t revision = 0;
    if (!worlds::snapshot(rows, count, revision)) {
        return {};
    }
    auto visible = std::span{rows}.first(count);
    std::ranges::stable_sort(visible, [](const worlds::Summary& left,
                                         const worlds::Summary& right) noexcept {
        const std::string_view leftStem = worlds::stem_of(left);
        const std::string_view rightStem = worlds::stem_of(right);
        if (leftStem != rightStem) {
            return leftStem < rightStem;
        }
        const std::string_view leftName = worlds::name_of(left);
        const std::string_view rightName = worlds::name_of(right);
        return leftName != rightName ? leftName < rightName
                                     : left.scenarioTag < right.scenarioTag;
    });
    return visible;
}

void select_activity(const worlds::Summary& summary) noexcept {
    location_catalog::ActivitySelection selection{};
    if (!activity_selection(summary, selection)
        || !location_catalog::select_activity(selection)) {
        return;
    }
    // An authored activity selection is primarily useful through its Logic placements. Make the
    // selection observable even when the user previously disabled the Logic filter.
    g_state.showLogic = true;
    clear_selection();
    g_state.session.reset_document();
    g_state.rowsValid = false;
}

void return_to_live_activity() noexcept {
    if (!location_catalog::clear_activity_preview()) {
        return;
    }
    clear_selection();
    g_state.session.reset_document();
    g_state.rowsValid = false;
}

void draw_activity_browser() noexcept {
    if (g_state.activityBrowserOpen) {
        ImGui::OpenPopup("Activity Browser");
        g_state.activityBrowserOpen = false;
    }
    ImGui::SetNextWindowSize({scaled(720.0F), scaled(520.0F)}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("Activity Browser")) {
        return;
    }
    const location_catalog::PreviewStatus preview = location_catalog::preview_status();
    ImGui::TextUnformatted("Installed activities");
    ImGui::SameLine();
    ImGui::TextDisabled("- package-native Graph, Logic, and placements");
    if (preview.active) {
        ImGui::TextColored(kWarning,
                           "AUTHORED PREVIEW - NOT LIVE: %s / 0x%08X",
                           preview.displayName.c_str(),
                           preview.scenarioTag);
        ImGui::TextDisabled("Graph: %s (%zu)  |  Logic: %s (%zu)",
                            location_catalog::state_name(preview.activityGraph.state),
                            preview.activityGraph.records,
                            location_catalog::state_name(preview.activityLogic.state),
                            preview.activityLogic.records);
        const std::string liveFamily = normalized_map_family(
            world_context().mapStem.empty() ? std::string_view(world_context().packageName)
                                            : std::string_view(world_context().mapStem));
        if (preview.mapFamily != liveFamily) {
            ImGui::TextColored(kWarning,
                               "Different map family: placements remain in the tree; viewport helpers are suppressed.");
        }
        if (control_button("Return to live activity", {0.0F, control_height()}, true, true)) {
            return_to_live_activity();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##activity_search",
                             "Search package, map family, or 0x scenario tag",
                             g_state.activitySearch.data(),
                             g_state.activitySearch.size());
    const std::span<const worlds::Summary> rows = activity_rows();
    const std::string_view query = g_state.activitySearch.data();
    std::size_t matches = 0;
    for (const worlds::Summary& row : rows) {
        matches += activity_matches(row, query) ? 1U : 0U;
    }
    ImGui::TextDisabled("%zu matches / %zu installed activities", matches, rows.size());
    ImGui::Separator();
    if (ImGui::BeginTable("##activity_table",
                          3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                              | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          {0.0F, (std::max)(scaled(300.0F), ImGui::GetContentRegionAvail().y)})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Map family");
        ImGui::TableSetupColumn("Activity package");
        ImGui::TableSetupColumn(
            "Scenario", ImGuiTableColumnFlags_WidthFixed, scaled(112.0F));
        ImGui::TableHeadersRow();
        for (const worlds::Summary& row : rows) {
            if (!activity_matches(row, query)) {
                continue;
            }
            ImGui::PushID(static_cast<int>(row.scenarioTag));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string_view stem = worlds::stem_of(row);
            if (stem.empty()) {
                ImGui::TextDisabled("(unknown)");
            } else {
                ImGui::TextUnformatted(stem.data(), stem.data() + stem.size());
            }
            ImGui::TableNextColumn();
            const std::string_view name = worlds::name_of(row);
            const bool selected = preview.active && preview.scenarioTag == row.scenarioTag;
            const std::string label = name.empty() ? std::string("(unnamed)") : std::string(name);
            if (ImGui::Selectable(label.c_str(),
                                  selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                select_activity(row);
                ImGui::CloseCurrentPopup();
            }
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", row.scenarioTag);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndPopup();
}

void draw_toolbar() noexcept {
    ImGui::BeginChild("##world_toolbar",
                      {0.0F, scaled(kToolbarHeight)},
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginMenuBar()) {
        const ImTextureID icon = textures::get(textures::Slot::inspectorIcon);
        if (icon != ImTextureID_Invalid) {
            ImGui::Image(icon, {scaled(18.0F), scaled(18.0F)});
        } else {
            ImGui::Dummy({scaled(18.0F), scaled(18.0F)});
        }
        ImGui::SameLine();

        const char* package =
            world_context().packageName.empty() ? "No world" : world_context().packageName.c_str();
        ImGui::TextUnformatted(package);
        if (world_context().bubble.has_value()) {
            ImGui::SameLine();
            ImGui::TextDisabled("/ Bubble %u", static_cast<unsigned>(*world_context().bubble));
        }
        if (world_context().activityIndex >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("/ Activity %d", world_context().activityIndex);
        }

        ImGui::Separator();
        if (ImGui::BeginMenu("Center view")) {
            const model::Node* selected = selected_node();
            const bool relationshipsAvailable =
                selected != nullptr
                && ((!selected->relations.empty())
                    || (selected->activityLogicMetadata.has_value()
                        && !selected->activityLogicMetadata->relationships.empty()));
            for (const WorkspaceViewDescriptor& view : kWorkspaceViews) {
                const bool available =
                    view.mode != CenterMode::relationships || relationshipsAvailable;
                ImGui::BeginDisabled(!available);
                if (ImGui::MenuItem(view.label, nullptr, g_state.centerMode == view.mode)) {
                    g_state.centerMode = view.mode;
                    if (view.mode == CenterMode::nodeGraph) {
                        g_state.ownershipGraphState.fitRequested = true;
                    } else if (view.mode == CenterMode::relationships) {
                        g_state.relationshipGraphState.fitRequested = true;
                    }
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s%s",
                                      view.summary,
                                      available ? ""
                                                : "\nNo relationships exist for the selection.");
                }
            }
            ImGui::Separator();
            ImGui::BeginDisabled(g_state.centerMode != CenterMode::overview);
            if (ImGui::MenuItem("Fit Overview")) {
                g_state.overviewGraphState.fitRequested = true;
            }
            if (ImGui::MenuItem("Center selected node##overview")) {
                g_state.overviewGraphState.centerRequested = g_state.selection.selected();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::BeginDisabled(g_state.centerMode != CenterMode::nodeGraph);
            if (ImGui::MenuItem("Fit Node Graph")) {
                g_state.ownershipGraphState.fitRequested = true;
            }
            if (ImGui::MenuItem("Center selected node")) {
                g_state.ownershipGraphState.centerRequested = g_state.selection.selected();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Selected neighborhood",
                                nullptr,
                                g_state.graphScope == GraphScope::selectionNeighborhood)) {
                g_state.graphScope = GraphScope::selectionNeighborhood;
                g_state.ownershipGraphState.fitRequested = true;
            }
            if (ImGui::MenuItem("All filtered nodes",
                                nullptr,
                                g_state.graphScope == GraphScope::filteredHierarchy)) {
                g_state.graphScope = GraphScope::filteredHierarchy;
                g_state.ownershipGraphState.fitRequested = true;
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Activities")) {
            const location_catalog::PreviewStatus preview = location_catalog::preview_status();
            if (preview.active) {
                ImGui::TextColored(kWarning, "AUTHORED PREVIEW - NOT LIVE");
                ImGui::TextDisabled("%s / 0x%08X",
                                    preview.displayName.c_str(),
                                    preview.scenarioTag);
                ImGui::TextDisabled("Graph: %s (%zu)",
                                    location_catalog::state_name(preview.activityGraph.state),
                                    preview.activityGraph.records);
                ImGui::TextDisabled("Logic: %s (%zu)",
                                    location_catalog::state_name(preview.activityLogic.state),
                                    preview.activityLogic.records);
                if (ImGui::MenuItem("Return to Live Activity")) {
                    return_to_live_activity();
                }
                ImGui::Separator();
            }
            std::array<char, 256> currentLabel{};
            std::snprintf(currentLabel.data(),
                          currentLabel.size(),
                          "Current live activity: %s",
                          world_context().packageName.empty()
                              ? "unresolved"
                              : world_context().packageName.c_str());
            if (ImGui::MenuItem(currentLabel.data(), nullptr, !preview.active)) {
                return_to_live_activity();
            }

            const std::span<const worlds::Summary> rows = activity_rows();
            const std::string currentFamily = normalized_map_family(
                world_context().mapStem.empty() ? std::string_view(world_context().packageName)
                                                : std::string_view(world_context().mapStem));
            if (!currentFamily.empty()
                && ImGui::BeginMenu((std::string("This map family: ") + currentFamily).c_str())) {
                std::size_t local = 0;
                for (const worlds::Summary& row : rows) {
                    const std::string family = normalized_map_family(
                        worlds::stem_of(row).empty() ? worlds::name_of(row) : worlds::stem_of(row));
                    if (family != currentFamily || row.scenarioTag == world_context().scenarioTag) {
                        continue;
                    }
                    ++local;
                    const std::string_view name = worlds::name_of(row);
                    std::array<char, 256> label{};
                    std::snprintf(label.data(),
                                  label.size(),
                                  "%.*s  [0x%08X]",
                                  static_cast<int>(name.size()),
                                  name.data(),
                                  row.scenarioTag);
                    if (ImGui::MenuItem(label.data(),
                                        nullptr,
                                        preview.active && preview.scenarioTag == row.scenarioTag)) {
                        select_activity(row);
                    }
                }
                if (local == 0) {
                    ImGui::TextDisabled("No other installed activities use this map family.");
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Browse All Activities...")) {
                g_state.activityBrowserOpen = true;
            }
            if (rows.empty()) {
                ImGui::TextDisabled("The runtime scenario roster is unavailable.");
            }
            ImGui::EndMenu();
        }
        const location_catalog::PreviewStatus toolbarPreview =
            location_catalog::preview_status();
        if (toolbarPreview.active) {
            ImGui::Separator();
            ImGui::TextColored(kWarning,
                               "AUTHORED PREVIEW - NOT LIVE: %s",
                               toolbarPreview.displayName.c_str());
        }
        if (ImGui::BeginMenu("Packages")) {
            location_catalog::Scope scope{};
            const bool scopeResolved = current_catalog_scope(scope);
            const location_catalog::Status catalogStatus = location_catalog::status();
            ImGui::BeginDisabled(!scopeResolved || !catalogStatus.canCollect);
            if (ImGui::MenuItem("Catalogue Current Location")) {
                (void)location_catalog::request(scope);
            }
            ImGui::EndDisabled();
            if (!scopeResolved) {
                ImGui::TextDisabled("Enter a resolved activity location to catalogue its packages.");
            }
            ImGui::Separator();
            const auto domain = [](const char* label,
                                   const location_catalog::DomainStatus& value) {
                ImGui::Text("%s: %s (%zu)",
                            label,
                            location_catalog::state_name(value.state),
                            value.records);
                if (!value.diagnostic.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", value.diagnostic.c_str());
                }
            };
            domain("Activity Graph", catalogStatus.activityGraph);
            domain("Activity Logic", catalogStatus.activityLogic);
            domain("Bubble bounds", catalogStatus.bubbleBounds);
            domain("Statics", catalogStatus.statics);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Runtime")) {
            if (ImGui::MenuItem(
                    "Continuously refresh membership", nullptr, &g_state.liveRuntimeMembership)) {
                g_state.session.set_live_runtime_membership(g_state.liveRuntimeMembership);
            }
            ImGui::TextDisabled("Research mode: observes bounded object, trigger, and physics");
            ImGui::TextDisabled("membership changes without requiring an activity reload.");
            if (g_state.liveRuntimeMembership) {
                ImGui::TextColored(kWarning, "Live membership may rebuild on busy maps.");
            }
            ImGui::Separator();
            ImGui::TextDisabled("Glimmer Test");
            static std::uint64_t entitySlotRepublishToken = 0;
            if (ImGui::MenuItem("Republish held entity slots once")) {
                entitySlotRepublishToken = roster_push::request_entity_slot_republish();
            }
            const auto entitySlotRepublish = roster_push::entity_slot_republish_status();
            ImGui::TextDisabled(
                "Entity slots: pending %llu, staged %llu, delivered %llu (last %llu)",
                static_cast<unsigned long long>(entitySlotRepublish.pendingToken),
                static_cast<unsigned long long>(entitySlotRepublish.stagedToken),
                static_cast<unsigned long long>(entitySlotRepublish.delivered),
                static_cast<unsigned long long>(entitySlotRepublish.deliveredToken));
            ImGui::TextDisabled(
                "Attempts req/bound/staged %llu/%llu/%llu, discard %llu, "
                "stale/public/no-private %llu/%llu/%llu, encode %llu",
                static_cast<unsigned long long>(entitySlotRepublish.requested),
                static_cast<unsigned long long>(entitySlotRepublish.bound),
                static_cast<unsigned long long>(entitySlotRepublish.staged),
                static_cast<unsigned long long>(entitySlotRepublish.discarded),
                static_cast<unsigned long long>(entitySlotRepublish.staleRejected),
                static_cast<unsigned long long>(entitySlotRepublish.publicRejected),
                static_cast<unsigned long long>(entitySlotRepublish.noPrivateRejected),
                static_cast<unsigned long long>(entitySlotRepublish.encodeFailed));
            if (entitySlotRepublishToken == 0
                && entitySlotRepublish.noPrivateRejected != 0) {
                ImGui::TextColored(kWarning, "Republish requires an exact private-current binding.");
            }
            ImGui::TextDisabled("Manual diagnostic only; duplicate type-0 semantics are unproven.");
            const auto reload =
                server::gameplay::physics::host::session::mission_reload_status();
            ImGui::BeginDisabled(reload.activeWorlds == 0);
            if (ImGui::MenuItem("Trigger Trostland Glimmer once")) {
                (void)server::gameplay::physics::host::session::request_mission_trigger(
                    kTrostlandEventVolume);
            }
            if (ImGui::MenuItem("Reload / re-arm active Lua mission")) {
                (void)server::gameplay::physics::host::session::request_mission_reload();
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Arm wide sobject code capture")) {
                (void)hooks::retail_log::rearm_sobject_capture();
            }
            static std::uint64_t manualCaptureRva = 0x35EE10;
            static std::uint64_t manualCaptureGeneration = 0;
            ImGui::SetNextItemWidth(120.0F);
            ImGui::InputScalar("Function RVA",
                               ImGuiDataType_U64,
                               &manualCaptureRva,
                               nullptr,
                               nullptr,
                               "%llX",
                               ImGuiInputTextFlags_CharsHexadecimal);
            if (ImGui::MenuItem("Capture function RVA")) {
                manualCaptureGeneration = hooks::retail_log::capture_sobject_function(
                    static_cast<std::uintptr_t>(manualCaptureRva));
            }
            if (manualCaptureGeneration != 0) {
                ImGui::TextDisabled("Manual code capture: %llu",
                                    static_cast<unsigned long long>(manualCaptureGeneration));
            }
            const auto codeCapture = hooks::retail_log::sobject_capture_status();
            ImGui::TextDisabled("Mission: state %u, objective %u, worlds %u",
                                reload.missionState,
                                reload.objective0,
                                reload.activeWorlds);
            ImGui::TextDisabled("Reload: %llu/%llu  trigger: %llu/%llu",
                                static_cast<unsigned long long>(reload.completed),
                                static_cast<unsigned long long>(reload.requested),
                                static_cast<unsigned long long>(reload.triggerCompleted),
                                static_cast<unsigned long long>(reload.triggerRequested));
            ImGui::TextDisabled("Program: 0x%016llX",
                                static_cast<unsigned long long>(reload.programHash));
            ImGui::TextDisabled("Code capture %llu: %s, background %u, recent %u",
                                static_cast<unsigned long long>(codeCapture.generation),
                                codeCapture.wideArmed
                                    ? "armed"
                                    : (codeCapture.wideCapturing
                                           ? "capturing"
                                           : (codeCapture.wideRearming ? "re-arming" : "captured")),
                                codeCapture.backgroundFailures,
                                codeCapture.recentFailures);
            ImGui::TextDisabled("Passive telemetry only; no lifecycle signal is emitted.");
            const auto spawner = hooks::squad_reference_probe::runtime_snapshot();
            ImGui::TextDisabled(
                "Spawner applies: %llu, resolves: %llu, requests: %llu, create outcomes: %llu",
                static_cast<unsigned long long>(spawner.applyCalls),
                static_cast<unsigned long long>(spawner.resolveCalls),
                static_cast<unsigned long long>(spawner.buildRequestCalls),
                static_cast<unsigned long long>(spawner.createOutcomeCalls));
            if (spawner.lastActiveInstance != 0) {
                ImGui::TextDisabled("Last active: 0x%llX  requested {%u,%u}  pending %u",
                                    static_cast<unsigned long long>(spawner.lastActiveInstance),
                                    spawner.requestedFirst,
                                    spawner.requestedSecond,
                                    spawner.pending);
            } else {
                ImGui::TextDisabled("No non-idle spawner state observed.");
            }
            if (spawner.decodedState != 0) {
                ImGui::TextDisabled(
                    "Decoded: count %u requested {%u,%u} gen %u mode %u active %u",
                    spawner.decodedSlotCount,
                    spawner.decodedRequestedFirst,
                    spawner.decodedRequestedSecond,
                    spawner.decodedGeneration,
                    static_cast<unsigned>(spawner.decodedMode),
                    spawner.decodedActive ? 1U : 0U);
            }
            const auto research = roster_push::trostland_spawner_research();
            ImGui::TextDisabled("Sense observed: gen %u delta 0x%08X",
                                research.observedGeneration,
                                research.observedDelta);
            static std::uint32_t manualGeneration = 0;
            ImGui::SetNextItemWidth(110.0F);
            ImGui::InputScalar("Manual generation", ImGuiDataType_U32, &manualGeneration);
            if (ImGui::MenuItem("Apply generation override")) {
                roster_push::set_trostland_spawner_generation(manualGeneration);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Overlays")) {
            const InspectorCapabilities available = capabilities();
            if (ImGui::BeginMenu("Helper detail")) {
                if (ImGui::MenuItem(
                        "Adaptive", nullptr, g_state.overlayDetail == viewport::Detail::adaptive)) {
                    g_state.overlayDetail = viewport::Detail::adaptive;
                    persist_layout();
                }
                if (ImGui::MenuItem("Camera nearby",
                                    nullptr,
                                    g_state.overlayDetail == viewport::Detail::cameraNearby)) {
                    g_state.overlayDetail = viewport::Detail::cameraNearby;
                    persist_layout();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Selected only",
                                    nullptr,
                                    g_state.overlayDetail == viewport::Detail::selectedOnly)) {
                    g_state.overlayDetail = viewport::Detail::selectedOnly;
                    persist_layout();
                }
                if (ImGui::MenuItem("Selected and nearby",
                                    nullptr,
                                    g_state.overlayDetail == viewport::Detail::selectedNearby)) {
                    g_state.overlayDetail = viewport::Detail::selectedNearby;
                    persist_layout();
                }
                if (ImGui::MenuItem(
                        "All admitted", nullptr, g_state.overlayDetail == viewport::Detail::all)) {
                    g_state.overlayDetail = viewport::Detail::all;
                    persist_layout();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Spawn helpers", nullptr, &g_state.showSpawns)) {
                g_state.rowsValid = false;
                persist_layout();
            }
            ImGui::BeginDisabled(available.logicPlacements == 0);
            if (ImGui::MenuItem("Authored activity placements", nullptr, &g_state.showLogic)) {
                g_state.rowsValid = false;
                persist_layout();
            }
            ImGui::EndDisabled();
            if (available.logicPlacements == 0
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("No exact WorldID-backed activity-logic placements are present "
                                  "for this scenario.");
            }
            ImGui::BeginDisabled(available.logicPlacements == 0);
            if (ImGui::MenuItem(
                    "Authored serialized orientation", nullptr, &g_state.showAuthoredOrientation)) {
                g_state.rowsValid = false;
                persist_layout();
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(available.knownBounds == 0);
            if (ImGui::MenuItem("Known bounds", nullptr, &g_state.showKnownBounds)) {
                persist_layout();
            }
            ImGui::EndDisabled();
            if (available.knownBounds == 0
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("No current producer supplies a validated AABB. The overlay "
                                  "never guesses bounds.");
            }
            ImGui::BeginDisabled(available.triggerCenters == 0);
            if (ImGui::MenuItem("Trigger centers", nullptr, &g_state.showTriggerCenters)) {
                persist_layout();
            }
            ImGui::EndDisabled();
            if (available.triggerCenters == 0
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("No trigger center observations are present in this snapshot.");
            }
            if (ImGui::MenuItem("Helper labels", nullptr, &g_state.showLabels)) {
                persist_layout();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Workspace")) {
            if (ImGui::MenuItem("Bottom dock", nullptr, !g_state.bottomCollapsed)) {
                g_state.bottomCollapsed = !g_state.bottomCollapsed;
                persist_layout();
            }
            if (ImGui::MenuItem("Focus search", "Ctrl+F")) {
                g_state.focusSearch = true;
            }
            if (ImGui::MenuItem("Reset layout")) {
                reset_layout();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Return to Sunrise")) {
                close();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    const ImVec2 minimum = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine({minimum.x, minimum.y + size.y - 1.0F},
                                        {minimum.x + size.x, minimum.y + size.y - 1.0F},
                                        ImGui::GetColorU32(kSpawn),
                                        1.0F);
    ImGui::EndChild();
}

SplitterResult splitter(const char* id,
                        float length,
                        bool vertical,
                        float& value,
                        float direction,
                        float minimum,
                        float maximum) noexcept {
    SplitterResult result{};
    const ImVec2 size = vertical ? ImVec2{scaled(kSplitterThickness), length}
                                 : ImVec2{length, scaled(kSplitterThickness)};
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiCol color = active ? ImGuiCol_SeparatorActive
                                  : (hovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(color), scaled(2.0F));
    if (hovered || active) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    if (active && maximum >= minimum) {
        const float delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0F) {
            const float next = std::clamp(value + delta * direction, minimum, maximum);
            result.changed = next != value;
            value = next;
            g_state.layoutDirty = g_state.layoutDirty || result.changed;
        }
    }
    result.released = ImGui::IsItemDeactivated();
    return result;
}

void finish_splitter(const SplitterResult& result) noexcept {
    if (result.released && g_state.layoutDirty) {
        persist_layout();
    }
}

void draw_status(const camera::Status& status) noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {scaled(10.0F), scaled(3.0F)});
    ImGui::BeginChild("##world_status",
                      {0.0F, scaled(kStatusHeight)},
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    constexpr const char* fullBadge = "READ-ONLY · PLAYER TELEPORT EXPLICIT";
    constexpr const char* compactBadge = "READ-ONLY";
    const float contentMinimum = ImGui::GetCursorScreenPos().x;
    const float contentMaximum = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float gap = scaled(12.0F);
    const float fullBadgeWidth = ImGui::CalcTextSize(fullBadge).x;
    const float minimumLeftWidth =
        ImGui::CalcTextSize("Camera -0000.00 -0000.00 -0000.00 | Faulted · Detached").x;
    const bool useFullBadge =
        contentMaximum - contentMinimum >= fullBadgeWidth + gap + minimumLeftWidth;
    const char* badge = useFullBadge ? fullBadge : compactBadge;
    const status_layout::Regions regions =
        status_layout::compute(contentMinimum, contentMaximum, gap, ImGui::CalcTextSize(badge).x);
    const ImVec2 lineOrigin = ImGui::GetCursorScreenPos();

    ImGui::PushClipRect(lineOrigin,
                        {regions.leftMaximum,
                         lineOrigin.y + (std::max)(control_height(), ImGui::GetTextLineHeight())},
                        true);
    std::array<char, 96> coordinates{};
    (void)std::snprintf(coordinates.data(),
                        coordinates.size(),
                        "Camera %.2f  %.2f  %.2f",
                        static_cast<double>(status.pose.position[0]),
                        static_cast<double>(status.pose.position[1]),
                        static_cast<double>(status.pose.position[2]));
    ImGui::TextUnformatted(coordinates.data());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click to copy camera coordinates");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            copy_camera_position(status.pose);
        }
    }

    const auto item_fits = [&regions](const char* label) noexcept {
        return status_layout::item_fits(ImGui::GetItemRectMax().x,
                                        ImGui::GetStyle().ItemSpacing.x,
                                        ImGui::CalcTextSize(label).x,
                                        regions.leftMaximum);
    };
    const auto disabled = [&item_fits](const char* label) noexcept {
        if (!item_fits(label)) {
            return false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", label);
        return true;
    };
    const auto colored = [&item_fits](const char* label, const ImVec4& color) noexcept {
        if (!item_fits(label)) {
            return false;
        }
        ImGui::SameLine();
        ImGui::TextColored(color, "%s", label);
        return true;
    };
    const auto inline_button = [&regions](const char* label) noexcept {
        const float buttonWidth =
            ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F;
        if (!status_layout::item_fits(ImGui::GetItemRectMax().x,
                                      ImGui::GetStyle().ItemSpacing.x,
                                      buttonWidth,
                                      regions.leftMaximum)) {
            return false;
        }
        ImGui::SameLine();
        return control_button(label, {buttonWidth, control_height()}, false, true);
    };

    std::array<char, 192> segment{};
    std::snprintf(segment.data(),
                  segment.size(),
                  "| %s · %s",
                  camera_phase_label(status.phase),
                  camera_source_label(status.poseSource));
    (void)colored(segment.data(), status.phase == camera::Phase::faulted ? kFailure : kSelection);

    const InspectorCapabilities available = capabilities();
    if (g_state.bottomCollapsed && inline_button("Show bottom")) {
        g_state.bottomCollapsed = false;
        persist_layout();
    }
    if (available.errors != 0 || available.warnings != 0) {
        std::snprintf(segment.data(),
                      segment.size(),
                      "| %zu errors / %zu warnings",
                      available.errors,
                      available.warnings);
        (void)colored(segment.data(), available.errors != 0 ? kFailure : kWarning);
    }
    const location_catalog::PreviewStatus preview = location_catalog::preview_status();
    if (preview.active) {
        std::snprintf(segment.data(),
                      segment.size(),
                      "| AUTHORED PREVIEW: %s · NOT LIVE",
                      preview.displayName.c_str());
        (void)colored(segment.data(), kWarning);
    }
    std::snprintf(segment.data(),
                  segment.size(),
                  "| %.0f FPS",
                  static_cast<double>(ImGui::GetIO().Framerate));
    (void)disabled(segment.data());
    std::snprintf(segment.data(),
                  segment.size(),
                  "| %zu nodes · %zu spatial · %zu bounds",
                  available.liveNodes,
                  available.spatialNodes,
                  available.knownBounds);
    (void)disabled(segment.data());
    if (available.logicDefinitions != 0 || available.logicVariables != 0) {
        std::snprintf(segment.data(),
                      segment.size(),
                      "| Logic %zu / %zu placed / %zu variables",
                      available.logicDefinitions,
                      available.logicPlacements,
                      available.logicVariables);
        (void)disabled(segment.data());
    }
    if (available.triggerCenters != 0) {
        std::snprintf(segment.data(),
                      segment.size(),
                      "| Triggers %zu centers / %zu shapes",
                      available.triggerCenters,
                      available.triggerShapes);
        (void)disabled(segment.data());
    }
    if (world_context().bubble.has_value()) {
        std::snprintf(segment.data(),
                      segment.size(),
                      "| Bubble %u",
                      static_cast<unsigned>(*world_context().bubble));
        (void)disabled(segment.data());
    }
    const model::Node* selected = selected_node();
    if (selected != nullptr) {
        std::snprintf(segment.data(), segment.size(), "| %s", model::kind_name(selected->kind));
        (void)disabled(segment.data());
    }
    ImGui::PopClipRect();

    ImGui::SetCursorScreenPos({regions.badgeMinimum, lineOrigin.y});
    ImGui::TextColored(kSelection, "%s", badge);
    ImGui::EndChild();
}

void push_workspace_style() noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, scaled(1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, scaled(1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, scaled(2.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, scaled(1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, scaled(1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {scaled(6.0F), scaled(4.0F)});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {scaled(6.0F), scaled(3.0F)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {scaled(8.0F), scaled(6.0F)});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, scaled(1.0F));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.010F, 0.009F, 0.015F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.91F, 0.91F, 0.88F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.48F, 0.48F, 0.46F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.024F, 0.024F, 0.033F, 0.985F));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.024F, 0.024F, 0.033F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25F, 0.21F, 0.14F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25F, 0.21F, 0.14F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.055F, 0.052F, 0.064F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.105F, 0.090F, 0.072F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.30F));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.18F));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.28F));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.38F));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.055F, 0.052F, 0.064F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.105F, 0.090F, 0.072F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.32F));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kSpawn);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.36F));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kSpawn);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.018F, 0.017F, 0.024F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.15F, 0.13F, 0.095F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.56F));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kSelection);
}

void pop_workspace_style() noexcept {
    ImGui::PopStyleColor(23);
    ImGui::PopStyleVar(9);
}

} // namespace

void initialize() noexcept {
    viewport::reset();
    g_state = {};
    g_open.store(false, std::memory_order_release);
    viewer_input::reset();
}

void shutdown() noexcept {
    camera::request_active(false);
    viewer_input::reset();
    g_open.store(false, std::memory_order_release);
    g_state.session.reset();
    viewport::reset();
    g_state = {};
}

void open() noexcept {
    camera::request_active(true);
    g_open.store(true, std::memory_order_release);
}

void close() noexcept {
    g_open.store(false, std::memory_order_release);
    camera::request_active(false);
    (void)location_catalog::clear_activity_preview();
    location_catalog::cancel();
    suspend();
}

void suspend() noexcept {
    g_state.viewportNavigation = false;
    g_state.session.clear_overlay();
    viewer_input::reset();
}

bool visible() noexcept {
    return g_open.load(std::memory_order_acquire);
}

EntrySnapshot entry_snapshot() noexcept {
    (void)g_state.session.refresh();
    EntrySnapshot result{};
    const auto add = [](ReadinessSummary& summary, const model::ProviderReport& report) noexcept {
        ++summary.producers;
        summary.readyProducers += report.ready ? 1U : 0U;
        summary.declared += report.declaredCount;
        summary.copied += report.copiedCount;
        summary.truncated = summary.truncated || report.truncated;
    };
    for (const model::ProviderReport& report : world().providerReports) {
        switch (report.producer) {
        case model::Producer::localPlayer:
        case model::Producer::objectSystem:
        case model::Producer::trigger:
        case model::Producer::audioListener:
        case model::Producer::physics:
            add(result.runtime, report);
            break;
        case model::Producer::placedContent:
        case model::Producer::spawnPoints:
        case model::Producer::staticFootprints:
        case model::Producer::bubbleBounds:
        case model::Producer::activityCatalog:
        case model::Producer::activityLogicCatalog:
            add(result.authored, report);
            break;
        default:
            break;
        }
    }
    const camera::Status cameraStatus = camera::status();
    const renderer::frame_capture::View frame = renderer::captured_frame_locked();
    result.rendering.producers = 2;
    result.rendering.readyProducers =
        (cameraStatus.phase == camera::Phase::active && cameraStatus.applied ? 1U : 0U)
        + (visible() && frame ? 1U : 0U);
    result.rendering.declared = 2;
    result.rendering.copied = result.rendering.readyProducers;
    result.packageName = world().context.packageName;
    result.mapStem = world().context.mapStem;
    result.activitySession = world().context.activitySession;
    result.valueRevision = world().valueRevision;
    result.sessionPresent = world().context.sessionPresent;
    result.stale = world().context.stale;
    return result;
}

void draw_launcher() noexcept {
    section::header("World Inspector",
                    "Inspect copied runtime observations and authored evidence for the current "
                    "world.");
    const EntrySnapshot snapshot = entry_snapshot();
    if (!snapshot.sessionPresent) {
        ImGui::TextDisabled("Waiting for a world session.");
    } else {
        ImGui::Text("Current world: %s%s",
                    snapshot.packageName.empty() ? "unresolved" : snapshot.packageName.c_str(),
                    snapshot.stale ? " (refreshing)" : "");
    }
    ImGui::Spacing();
    if (ImGui::Button("Open World Inspector")) {
        open();
    }
    ImGui::Spacing();
    client::player::Settings presentation = client::player::get();
    bool changed = toggle::control("Hide HUD##inspector_entry", presentation.removeHud);
    changed = toggle::control("Hide weapon##inspector_entry", presentation.hideWeapon)
              || changed;
    if (changed) {
        (void)client::player::publish(presentation);
    }
}

bool render(bool uiVisible) noexcept {
    if (!visible()) {
        g_state.viewportNavigation = false;
        g_state.session.clear_overlay();
        viewer_input::reset();
        return false;
    }
    if (!uiVisible) {
        g_state.viewportNavigation = false;
        g_state.session.clear_overlay();
        viewer_input::reset();
        return false;
    }

    initialize_layout();
    refresh_layout_scale();
    const model::NodeId previousSelection = g_state.selection.selected();
    if (!g_state.selectedKey) {
        g_state.selectedKey = world().graph.key(previousSelection);
    }
    const provider::RefreshResult refresh = g_state.session.refresh();
    const bool rebuilt = refresh.rebuilt();

    location_catalog::Scope catalogScope{};
    (void)current_catalog_scope(catalogScope);
    if (location_catalog::refresh(catalogScope)) {
        g_state.session.reset_document();
        g_state.rowsValid = false;
    }
    if (rebuilt) {
        const bool firstPopulation = !g_state.stableStateInitialized;
        restore_stable_state(firstPopulation);
        g_state.stableStateInitialized = true;
        ++g_state.hiddenRevision;
        g_state.restoreTreeScroll = true;
        g_state.rowsValid = false;
    }
    g_state.selection.clear_hover();

    const camera::Status cameraStatus = camera::status();
    const renderer::frame_capture::View capturedFrame = renderer::captured_frame_locked();
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr || mainViewport->Size.x <= 0.0F || mainViewport->Size.y <= 0.0F) {
        g_state.session.clear_overlay();
        return false;
    }

    push_workspace_style();
    ImGui::SetNextWindowPos(mainViewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(mainViewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    const bool submit = ImGui::Begin("World Inspector", nullptr, kWorkspaceFlags);
    ImGui::PopStyleVar(3);
    if (submit) {
        draw_toolbar();
        draw_activity_browser();

        const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
        const float statusHeight = scaled(kStatusHeight);
        const float splitterSize = scaled(kSplitterThickness);
        const float availableHeight = (std::max)(0.0F, ImGui::GetContentRegionAvail().y);
        const float availableWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);

        const VerticalLayout vertical =
            compute_vertical_layout(availableHeight,
                                    statusHeight,
                                    splitterSize,
                                    scaled(kMinimumMainHeight),
                                    g_state.bottomHeight,
                                    scaled(kMinimumBottomUsableHeight),
                                    scaled(model::settings::kMinimumBottomHeight),
                                    g_state.bottomCollapsed);
        const bool bottomVisible = vertical.bottomVisible;
        const float minimumBottom = vertical.minimumBottom;
        const float maximumVisibleBottom = vertical.maximumBottom;
        const float bottomHeight = vertical.bottomHeight;
        const float mainHeight = vertical.mainHeight;

        const float minimumLeft = scaled(model::settings::kMinimumLeftWidth);
        const float maximumLeft = scaled(model::settings::kMaximumLeftWidth);
        const float minimumRight = scaled(model::settings::kMinimumRightWidth);
        const float maximumRight = scaled(model::settings::kMaximumRightWidth);
        const float minimumCenter = scaled(kMinimumViewportWidth);
        const float horizontalBudget = (std::max)(1.0F, availableWidth - splitterSize * 2.0F);

        float leftWidth = std::clamp(g_state.leftWidth, minimumLeft, maximumLeft);
        float rightWidth = std::clamp(g_state.rightWidth, minimumRight, maximumRight);
        const bool sideMinimumsFit = horizontalBudget >= minimumCenter + minimumLeft + minimumRight;
        if (sideMinimumsFit) {
            const float leftLimit =
                (std::min)(maximumLeft, horizontalBudget - minimumCenter - minimumRight);
            leftWidth = std::clamp(leftWidth, minimumLeft, leftLimit);
            const float rightLimit =
                (std::min)(maximumRight, horizontalBudget - minimumCenter - leftWidth);
            rightWidth = std::clamp(rightWidth, minimumRight, rightLimit);
        } else {
            const float sideBudget = (std::max)(0.0F, horizontalBudget - 1.0F);
            const float preferredTotal = (std::max)(1.0F, leftWidth + rightWidth);
            const float leftRatio = std::clamp(leftWidth / preferredTotal, 0.2F, 0.8F);
            leftWidth = sideBudget * leftRatio;
            rightWidth = (std::max)(0.0F, sideBudget - leftWidth);
        }

        ImGui::SetCursorScreenPos(contentOrigin);
        ImGui::BeginChild("##world_outliner",
                          {leftWidth, mainHeight},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_outliner();
        ImGui::EndChild();

        ImGui::SetCursorScreenPos({contentOrigin.x + leftWidth, contentOrigin.y});
        const float leftDragMaximum =
            sideMinimumsFit ? (std::min)(maximumLeft, horizontalBudget - minimumCenter - rightWidth)
                            : 0.0F;
        const SplitterResult leftResult = splitter(
            "##left_splitter", mainHeight, true, leftWidth, 1.0F, minimumLeft, leftDragMaximum);
        if (leftResult.changed) {
            g_state.leftWidth = leftWidth;
        }
        finish_splitter(leftResult);
        const float centerWidth = (std::max)(1.0F, horizontalBudget - leftWidth - rightWidth);

        const float viewportX = contentOrigin.x + leftWidth + splitterSize;
        ImGui::SetCursorScreenPos({viewportX, contentOrigin.y});
        const bool worldCenter = g_state.centerMode == CenterMode::world;
        const ImVec4 viewportBackground = capturedFrame && worldCenter
                                              ? ImVec4(0.010F, 0.009F, 0.015F, 1.0F)
                                              : ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, viewportBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::BeginChild("##world_viewport",
                          {centerWidth, mainHeight},
                          ImGuiChildFlags_Borders,
                          capturedFrame && worldCenter ? ImGuiWindowFlags_None
                                                       : ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        viewport::Result interaction{};
        if (g_state.centerMode == CenterMode::overview) {
            const graph::Result graphInteraction = graph::draw_overview(
                world().graph,
                g_state.overviewAdmitted,
                g_state.admissionRevision,
                current_activity_scope(),
                static_cast<std::size_t>(g_state.maximumVisibleNodes),
                g_state.selection.selected(),
                g_state.overviewGraphState);
            interaction.hovered = graphInteraction.hovered;
            interaction.selected = graphInteraction.selected;
            interaction.context = graphInteraction.context;
            interaction.clearSelection = graphInteraction.clearSelection;
        } else if (worldCenter) {
            draw_helper_controls();
            viewport::Options overlayOptions = viewport_options(workspace_overlay_policy());
            overlayOptions.renderStatus = debug_scene::status();
            overlayOptions.depthGeometryReady =
                model::helper_geometry_current(overlayOptions.renderStatus, capturedFrame.frameId);
            overlayOptions.exactView = renderer::native_debug::view();
            overlayOptions.presentation = debug_scene::frame();
            interaction = viewport::draw(world().graph,
                                         g_state.selection.selected(),
                                         cameraStatus,
                                         capturedFrame,
                                         overlayOptions,
                                         g_state.viewportNavigation);
            g_state.lastViewportResult = interaction;
            const ui_memory::Stats uiMemory = ui_memory::snapshot();
            const bool allocatorChanged =
                uiMemory.arenaMisses != g_state.lastLoggedArenaMisses
                || uiMemory.allocationFailures != g_state.lastLoggedAllocationFailures;
            if (allocatorChanged) {
                const auto now = std::chrono::steady_clock::now();
                constexpr auto interval = std::chrono::seconds(5);
                if (g_state.lastAllocatorLog.time_since_epoch().count() == 0
                    || now - g_state.lastAllocatorLog >= interval) {
                    std::array<char, 256> event{};
                    const int written = std::snprintf(
                        event.data(),
                        event.size(),
                        "ev=inspector_ui_memory arena_misses=%zu spill_bytes=%zu "
                        "spill_high_water=%zu allocation_failures=%zu last_failed_bytes=%zu",
                        uiMemory.arenaMisses,
                        uiMemory.spillOutstandingBytes,
                        uiMemory.spillHighWaterBytes,
                        uiMemory.allocationFailures,
                        uiMemory.lastFailedBytes);
                    if (written > 0) {
                        core::log::write(core::log::Channel::client,
                                         uiMemory.allocationFailures == 0 ? core::log::Level::info
                                                                          : core::log::Level::warn,
                                         event.data());
                    }
                    g_state.lastAllocatorLog = now;
                    g_state.lastLoggedArenaMisses = uiMemory.arenaMisses;
                    g_state.lastLoggedAllocationFailures = uiMemory.allocationFailures;
                }
            }
            if (interaction.allocationFailure) {
                const auto now = std::chrono::steady_clock::now();
                constexpr auto interval = std::chrono::seconds(5);
                if (g_state.lastOverlayFailureLog.time_since_epoch().count() == 0
                    || now - g_state.lastOverlayFailureLog >= interval) {
                    core::log::write(
                        core::log::Channel::client,
                        core::log::Level::warn,
                        "ev=inspector_overlay result=allocation_failure frame=skipped");
                    g_state.lastOverlayFailureLog = now;
                }
            }
        } else if (g_state.centerMode == CenterMode::nodeGraph) {
            const model::NodeId graphRoot = prepare_graph_admission();
            const std::uint64_t graphRevision =
                g_state.admissionRevision ^ g_state.selection.selected().value
                ^ (g_state.graphScope == GraphScope::selectionNeighborhood ? 0x8000000000000000ULL
                                                                           : 0ULL);
            const graph::Result graphInteraction = graph::draw(world().graph,
                                                               graphRoot,
                                                               g_state.selection.selected(),
                                                               g_state.graphAdmitted,
                                                               graphRevision,
                                                               g_state.graphOmitted,
                                                               g_state.ownershipGraphState);
            interaction.hovered = graphInteraction.hovered;
            interaction.selected = graphInteraction.selected;
            interaction.context = graphInteraction.context;
            interaction.clearSelection = graphInteraction.clearSelection;
        } else if (g_state.centerMode == CenterMode::relationships) {
            const graph::Result graphInteraction = graph::draw_activity_logic_relationships(
                world().graph, g_state.selection.selected(), g_state.relationshipGraphState);
            interaction.hovered = graphInteraction.hovered;
            interaction.selected = graphInteraction.selected;
            interaction.context = graphInteraction.context;
            interaction.clearSelection = graphInteraction.clearSelection;
        } else {
            interaction = draw_activity_map();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        g_state.viewportNavigation = interaction.navigation;
        viewer_input::set_workspace_navigation(g_state.viewportNavigation);
        if (interaction.hovered) {
            g_state.selection.hover(interaction.hovered);
        }
        if (interaction.clearSelection) {
            clear_selection();
            g_state.revealSelection = false;
        }
        if (interaction.selected) {
            select_node(interaction.selected);
        }
        if (interaction.focused) {
            if (g_state.centerMode == CenterMode::world) {
                select_and_focus(interaction.focused);
            } else {
                select_node(interaction.focused);
            }
        }
        if (interaction.context) {
            select_node(interaction.context);
            g_state.contextTarget = interaction.context;
            g_state.contextRequested = true;
        }

        const float rightSplitterX = viewportX + centerWidth;
        ImGui::SetCursorScreenPos({rightSplitterX, contentOrigin.y});
        const float rightDragMaximum =
            sideMinimumsFit ? (std::min)(maximumRight, horizontalBudget - minimumCenter - leftWidth)
                            : 0.0F;
        const SplitterResult rightResult = splitter("##right_splitter",
                                                    mainHeight,
                                                    true,
                                                    rightWidth,
                                                    -1.0F,
                                                    minimumRight,
                                                    rightDragMaximum);
        if (rightResult.changed) {
            g_state.rightWidth = rightWidth;
        }
        finish_splitter(rightResult);

        ImGui::SetCursorScreenPos({rightSplitterX + splitterSize, contentOrigin.y});
        ImGui::BeginChild("##world_properties",
                          {rightWidth, mainHeight},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_inspector();
        ImGui::EndChild();

        if (bottomVisible) {
            ImGui::SetCursorScreenPos({contentOrigin.x, contentOrigin.y + vertical.splitterY});
            float draggedBottomHeight = bottomHeight;
            const SplitterResult bottomResult = splitter("##bottom_splitter",
                                                         availableWidth,
                                                         false,
                                                         draggedBottomHeight,
                                                         -1.0F,
                                                         minimumBottom,
                                                         maximumVisibleBottom);
            if (bottomResult.changed) {
                // Keep this frame internally consistent. The new height is consumed by the
                // next frame after every panel has finished using the current geometry.
                g_state.bottomHeight = draggedBottomHeight;
            }
            finish_splitter(bottomResult);

            ImGui::SetCursorScreenPos({contentOrigin.x, contentOrigin.y + vertical.bottomY});
            ImGui::BeginChild("##world_bottom",
                              {availableWidth, bottomHeight},
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            draw_bottom_dock();
            ImGui::EndChild();
        }

        ImGui::SetCursorScreenPos({contentOrigin.x, contentOrigin.y + vertical.statusY});
        draw_status(cameraStatus);

        if (g_state.contextRequested) {
            ImGui::OpenPopup("##world_inspector_context");
            g_state.contextRequested = false;
        }
        draw_node_context_menu();
        handle_shortcuts();
    }
    ImGui::End();
    pop_workspace_style();
    publish_scene_frame(cameraStatus, capturedFrame);
    return true;
}

bool selected_identity(SelectionIdentity& identity) noexcept {
    const model::Node* node = selected_node();
    if (node == nullptr) {
        identity = {};
        return false;
    }
    identity.producerEpoch = producer_epoch();
    identity.nativeKey = capture::stable_native_key(world(), *node);
    identity.producer = static_cast<std::uint32_t>(node->producer);
    identity.kind = static_cast<std::uint32_t>(node->kind);
    return identity.nativeKey != 0;
}

void service_camera_path_captures() noexcept {
    camera::SnapshotCaptureRequest request{};
    while (camera::consume_snapshot_capture_request(request)) {
        const provider::RefreshResult refresh = g_state.session.refresh();
        const bool rebuilt = refresh.rebuilt();
        if (rebuilt) {
            restore_stable_state(!g_state.stableStateInitialized);
            g_state.stableStateInitialized = true;
            ++g_state.hiddenRevision;
            g_state.rowsValid = false;
        }
        capture::RouteCaptureMetadata metadata{};
        metadata.pathName = request.pathName.data();
        metadata.keyframeLabel = request.keyframeLabel.data();
        metadata.position = request.pose.position;
        metadata.cameraSession = request.cameraSession;
        metadata.captureSequence = request.sequence;
        metadata.keyframeIndex = request.keyframeIndex;
        metadata.yaw = request.pose.yaw;
        metadata.pitch = request.pose.pitch;
        metadata.fov = request.pose.fov;
        g_state.session.export_route(metadata);
    }
}

// ---------------------------------------------------------------------------
// In-render debug scene helpers. These feed the renderer's depth-tested line pass
// with world-space segments derived from the same admitted set the viewport uses.
// ---------------------------------------------------------------------------

bool debug_scene::enabled() noexcept {
    return visible();
}

void debug_scene::publish_status(const DepthStatus& status) noexcept {
    g_state.session.publish_depth_status(status);
}

debug_scene::DepthStatus debug_scene::status() noexcept {
    return g_state.session.depth_status();
}

model::SceneFramePtr debug_scene::frame() noexcept {
    return g_state.session.overlay_frame();
}

} // namespace sunrise::client::ui::world_inspector
