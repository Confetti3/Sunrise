#include "world_inspector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/modules/logs/logs.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../core/ui/textures/ui_texture_slots.h"
#include "../../hooks/graphics/renderer/renderer.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../inspection/providers/spawn_inspection_provider.h"
#include "../../inspection/inspection_capture.h"
#include "../../inspection/world_inspection_model.h"
#include "../../viewer/viewer_camera_settings_store.h"
#include "../../viewer/viewer_camera_path_store.h"
#include "../../viewer/viewer_input_ownership.h"
#include "world_inspector_viewport.h"
#include "world_inspector_graph.h"

namespace sunrise::client::ui::world_inspector {
namespace {

namespace camera = client::viewer::camera;
namespace clipboard = core::ui::modules::logs;
namespace dpi = core::ui::scaling::dpi;
namespace model = client::inspection;
namespace capture = client::inspection::capture;
namespace provider = client::inspection::providers;
namespace activity_catalog = client::inspection::activity_catalog;
namespace renderer = client::hooks::graphics::renderer;
namespace section = core::ui::components::section;
namespace textures = core::ui::textures;
namespace viewer_input = client::viewer::input;
namespace camera_paths = client::viewer::paths;

constexpr float kToolbarHeight = 30.0F;
constexpr float kStatusHeight = 30.0F;
constexpr float kSplitterThickness = 5.0F;
constexpr float kMinimumViewportWidth = 360.0F;
constexpr float kMinimumMainHeight = 140.0F;
constexpr float kMinimumBottomUsableHeight = 40.0F;
constexpr float kMinimumTreeRowHeight = 28.0F;
constexpr float kInspectorRowPadding = 6.0F;
constexpr float kTreeIndent = 14.0F;
constexpr float kControlHeight = 24.0F;
constexpr std::size_t kSearchCapacity = 256;
constexpr ImU32 kGuideColor = IM_COL32(63, 55, 42, 210);
constexpr ImVec4 kSelection{0.796F, 0.608F, 0.318F, 1.0F};
constexpr ImVec4 kSpawn{0.965F, 0.886F, 0.478F, 1.0F};
constexpr ImVec4 kWarning{0.949F, 0.549F, 0.216F, 1.0F};
constexpr ImVec4 kFailure{0.941F, 0.349F, 0.349F, 1.0F};
constexpr ImVec4 kMuted{0.50F, 0.55F, 0.61F, 1.0F};

constexpr ImGuiWindowFlags kWorkspaceFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

[[nodiscard]] constexpr float padded_row_height(float textHeight,
                                                float verticalPadding,
                                                float minimumHeight) noexcept {
    return (std::max)(minimumHeight, textHeight + verticalPadding * 2.0F);
}

[[nodiscard]] constexpr bool fully_visible(float minimum,
                                           float maximum,
                                           float clipMinimum,
                                           float clipMaximum) noexcept {
    return minimum >= clipMinimum && maximum <= clipMaximum;
}

[[nodiscard]] constexpr float snap_scroll(float value,
                                          float rowHeight,
                                          float maximum) noexcept {
    const float clamped = std::clamp(value, 0.0F, (std::max)(0.0F, maximum));
    if (rowHeight <= 0.0F) {
        return clamped;
    }
    const auto row = static_cast<std::uint64_t>(clamped / rowHeight + 0.5F);
    return (std::min)(static_cast<float>(row) * rowHeight, maximum);
}

[[nodiscard]] constexpr float aligned_scroll_padding(float contentHeight,
                                                      float visibleHeight,
                                                      float rowHeight) noexcept {
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
    const float maximumBottom =
        (std::max)(0.0F, statusY - minimumMainHeight - splitterSize);
    const bool visible = !collapsed && maximumBottom >= minimumUsableBottomHeight;
    if (!visible) {
        return VerticalLayout{(std::max)(1.0F, statusY),
                              statusY,
                              statusY,
                              0.0F,
                              statusY,
                              0.0F,
                              0.0F,
                              false};
    }

    const float minimumBottom = (std::min)(preferredMinimumBottomHeight, maximumBottom);
    const float bottomHeight =
        std::clamp(requestedBottomHeight, minimumBottom, maximumBottom);
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

enum class HierarchyMode : std::uint8_t {
    world,
    source,
    activity,
};
enum class CenterMode : std::uint8_t {
    world,
    nodeGraph,
    activityMap,
};

enum class GraphScope : std::uint8_t {
    selectionNeighborhood,
    filteredHierarchy,
};

enum class BottomTab : std::uint8_t {
    references,
    data,
    events,
    compare,
    diagnostics,
};

enum class FilterGroup : std::uint8_t {
    geometry,
    entities,
    spawns,
    logic,
    triggers,
    audio,
    rendering,
    navigation,
};

struct TreeRow final {
    model::NodeId id{};
    std::string label;
    std::uint8_t depth{};
    bool hasChildren{};
};

struct NodeIdentity final {
    model::NodeKind kind{model::NodeKind::unresolved};
    std::optional<std::uint64_t> runtimeEntity;
    std::optional<std::uint64_t> observationId;
    std::optional<std::uint8_t> objectSystemType;
    std::optional<std::uint64_t> activitySession;
    std::optional<std::uint32_t> scenarioTag;
    std::string name;
    std::string searchText;
    bool group{};
    bool valid{};
};

struct SplitterResult final {
    bool changed{};
    bool released{};
};

struct WorkspaceState final {
    provider::SpawnInspectionProvider provider;
    capture::History history;
    std::optional<capture::InspectionSnapshot> comparisonBaseline;
    std::vector<capture::ChangeEvent> comparisonEvents;
    capture::ExportResult lastExport{};
    model::Selection selection;
    std::unordered_set<std::uint64_t> hidden;
    std::unordered_set<std::uint64_t> collapsed;
    std::vector<TreeRow> rows;
    std::unordered_set<std::uint64_t> admitted;
    std::unordered_set<std::uint64_t> graphAdmitted;
    std::uint64_t admissionRevision{};
    graph::State graphState;
    std::array<char, kSearchCapacity> search{};
    std::array<char, kSearchCapacity> eventFilter{};
    CenterMode centerMode{CenterMode::world};
    GraphScope graphScope{GraphScope::selectionNeighborhood};
    std::uint32_t selectedActivityGraphHash{};
    std::string cachedSearch;
    model::NodeId contextTarget{};
    HierarchyMode hierarchyMode{HierarchyMode::world};
    HierarchyMode cachedMode{HierarchyMode::world};
    BottomTab bottomTab{BottomTab::references};
    std::uint32_t cachedGeneration{};
    std::uint64_t hiddenRevision{};
    std::uint64_t cachedHiddenRevision{};
    float leftWidth{};
    float rightWidth{};
    float bottomHeight{};
    float layoutScale{1.0F};
    bool layoutInitialized{};
    bool rowsValid{};
    bool showGeometry{true};
    bool showEntities{true};
    bool showSpawns{true};
    bool showLogic{true};
    bool showTriggers{true};
    bool showAudio{true};
    bool showKnownBounds{true};
    bool showTriggerCenters{true};
    bool showRendering{true};
    bool showNavigation{true};
    bool showHidden{true};
    bool errorsOnly{};
    bool showLabels{};
    bool trackRuntimeOnly{true};
    bool trackTransforms{};
    bool bottomCollapsed{};
    bool viewportNavigation{};
    bool layoutDirty{};
    bool contextRequested{};
    bool focusSearch{};
    bool revealSelection{};
    NodeIdentity treeAnchor;
    float treeAnchorOffset{};
    bool restoreTreeScroll{};
};

std::atomic_bool g_open{};
WorkspaceState g_state{};

[[nodiscard]] float scaled(float value) noexcept {
    return dpi::pixels(value);
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

[[nodiscard]] const provider::WorldSnapshot& world() noexcept {
    return g_state.provider.snapshot();
}

struct InspectorCapabilities final {
    std::size_t liveNodes{};
    std::size_t spatialNodes{};
    std::size_t knownBounds{};
    std::size_t triggerCenters{};
    std::size_t triggerShapes{};
    std::size_t logicDefinitions{};
    std::size_t logicPlacements{};
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

[[nodiscard]] model::NodeId first_observation_node(
    const provider::WorldSnapshot& snapshot) noexcept {
    if (snapshot.runtimeObjectGroupNode) {
        return snapshot.runtimeObjectGroupNode;
    }
    if (snapshot.audioListenerNode) {
        return snapshot.audioListenerNode;
    }
    if (snapshot.physicsGroupNode) {
        return snapshot.physicsGroupNode;
    }
    return snapshot.physicsBodyNodes.empty() ? model::NodeId{} : snapshot.physicsBodyNodes.front();
}

[[nodiscard]] const model::Node* selected_node() noexcept {
    return world().graph.node(g_state.selection.selected());
}

[[nodiscard]] NodeIdentity node_identity(const model::Node* node) {
    if (node == nullptr) {
        return {};
    }
    return NodeIdentity{node->kind,
                        node->runtimeEntity,
                        node->observationId,
                        node->objectSystemType,
                        node->source.activitySession,
                        node->source.scenarioTag,
                        node->name,
                        node->searchText,
                        !node->children.empty(),
                        true};
}

[[nodiscard]] bool identity_matches(const NodeIdentity& identity,
                                    const model::Node& node) noexcept {
    if (!identity.valid || identity.kind != node.kind
        || identity.activitySession != node.source.activitySession
        || identity.scenarioTag != node.source.scenarioTag) {
        return false;
    }
    if (identity.runtimeEntity.has_value()) {
        return identity.runtimeEntity == node.runtimeEntity
               && identity.objectSystemType == node.objectSystemType;
    }
    if (identity.observationId.has_value() && node.kind != model::NodeKind::physics) {
        return identity.observationId == node.observationId;
    }
    if (identity.group) {
        return !node.children.empty() && identity.searchText == node.searchText;
    }
    return identity.name == node.name;
}

[[nodiscard]] model::NodeId find_node(const NodeIdentity& identity) noexcept {
    const auto iterator =
        std::ranges::find_if(world().graph.nodes(), [&identity](const model::Node& node) {
            return identity_matches(identity, node);
        });
    return iterator == world().graph.nodes().end() ? model::NodeId{} : iterator->id;
}

void copy_text(std::string_view text) noexcept {
    if (!text.empty()) {
        (void)clipboard::queue_text_copy(text);
    }
}

void copy_id(model::NodeId id) noexcept {
    std::array<char, 32> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "0x%016llX",
                                      static_cast<unsigned long long>(id.value));
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        copy_text(std::string_view(text.data(), static_cast<std::size_t>(written)));
    }
}

void copy_tag(std::uint32_t tag) noexcept {
    std::array<char, 16> text{};
    const int written = std::snprintf(text.data(), text.size(), "0x%08X", tag);
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        copy_text(std::string_view(text.data(), static_cast<std::size_t>(written)));
    }
}

void copy_position(const model::Transform& transform) noexcept {
    std::array<char, 128> text{};
    const auto& position = transform.position;
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "%.6f, %.6f, %.6f",
                                      static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2]));
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        copy_text(std::string_view(text.data(), static_cast<std::size_t>(written)));
    }
}

void copy_camera_position(const camera::Pose& pose) noexcept {
    model::Transform transform{};
    transform.position = pose.position;
    copy_position(transform);
}

[[nodiscard]] bool hidden(model::NodeId id) noexcept {
    return g_state.hidden.contains(id.value);
}

void note_visibility_change() noexcept {
    ++g_state.hiddenRevision;
    g_state.rowsValid = false;
}

void show_all() noexcept {
    if (!g_state.hidden.empty()) {
        g_state.hidden.clear();
        note_visibility_change();
    }
}

void toggle_hidden(model::NodeId id) noexcept {
    const model::Node* node = world().graph.node(id);
    if (node == nullptr || !model::supports(node->actions, model::Action::hide)) {
        return;
    }
    if (g_state.hidden.contains(id.value)) {
        g_state.hidden.erase(id.value);
    } else {
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
    for (const model::Node& node : world().graph.nodes()) {
        if (model::supports(node.actions, model::Action::hide) && node.id != id) {
            g_state.hidden.insert(node.id.value);
        }
    }
    note_visibility_change();
}

void focus(model::NodeId id) noexcept {
    const model::Node* node = world().graph.node(id);
    const camera::Status status = camera::status();
    if (node == nullptr || !status.active || !model::supports(node->actions, model::Action::focus)
        || (!node->transform.has_value()
            && !(node->bounds.has_value() && model::bounds_valid(*node->bounds)))) {
        return;
    }
    std::array<float, 3> target{};
    float distance = 6.0F;
    if (node->bounds.has_value() && model::bounds_valid(*node->bounds)) {
        target = model::bounds_center(*node->bounds);
        const auto extents = model::bounds_extents(*node->bounds);
        const float radius =
            (std::max)({extents[0], extents[1], extents[2], 0.0F});
        const float halfFov = status.pose.fov <= 3.2415927F
                                  ? status.pose.fov * 0.5F
                                  : status.pose.fov * (3.1415927F / 360.0F);
        const float tangent = std::tan(halfFov);
        if (std::isfinite(radius) && std::isfinite(tangent) && tangent > 0.05F) {
            distance = (std::max)(distance, radius / tangent * 2.0F);
        }
    } else {
        target = node->transform->position;
    }
    camera::Pose pose = status.pose;
    for (std::size_t lane = 0; lane < pose.position.size(); ++lane) {
        pose.position[lane] = target[lane] - pose.forward[lane] * distance;
    }
    (void)camera::move_to(pose);
}

void select_node(model::NodeId id) noexcept {
    if (const model::Node* node = world().graph.node(id); node != nullptr) {
        g_state.selection.select(id);
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
    client::viewer::Settings settings = client::viewer::get();
    settings.inspectorLeftWidth = std::clamp(g_state.leftWidth / scale,
                                             client::viewer::kMinimumInspectorLeftWidth,
                                             client::viewer::kMaximumInspectorLeftWidth);
    settings.inspectorRightWidth = std::clamp(g_state.rightWidth / scale,
                                              client::viewer::kMinimumInspectorRightWidth,
                                              client::viewer::kMaximumInspectorRightWidth);
    settings.inspectorBottomHeight = std::clamp(g_state.bottomHeight / scale,
                                                client::viewer::kMinimumInspectorBottomHeight,
                                                client::viewer::kMaximumInspectorBottomHeight);
    settings.inspectorBottomCollapsed = g_state.bottomCollapsed;
    (void)client::viewer::publish(settings);
    g_state.layoutDirty = false;
}

void reset_layout() noexcept {
    g_state.leftWidth = scaled(client::viewer::kDefaultInspectorLeftWidth);
    g_state.rightWidth = scaled(client::viewer::kDefaultInspectorRightWidth);
    g_state.bottomHeight = scaled(client::viewer::kDefaultInspectorBottomHeight);
    g_state.bottomCollapsed = false;
    g_state.layoutScale = (std::max)(dpi::current(), 0.01F);
    g_state.layoutDirty = true;
    persist_layout();
}

void initialize_layout() noexcept {
    if (g_state.layoutInitialized) {
        return;
    }
    const client::viewer::Settings settings = client::viewer::get();
    g_state.leftWidth = scaled(settings.inspectorLeftWidth);
    g_state.rightWidth = scaled(settings.inspectorRightWidth);
    g_state.bottomHeight = scaled(settings.inspectorBottomHeight);
    g_state.bottomCollapsed = settings.inspectorBottomCollapsed;
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
                                     const provider::WorldSnapshot& snapshot) {
    std::array<char, 128> text{};
    switch (mode) {
    case HierarchyMode::world:
        return snapshot.packageName.empty() ? std::string("World") : snapshot.packageName;
    case HierarchyMode::source: {
        const int written = snapshot.scenarioTag == 0
                                ? std::snprintf(text.data(), text.size(), "Source / unresolved")
                                : std::snprintf(text.data(),
                                                text.size(),
                                                "Source / scenario 0x%08X",
                                                snapshot.scenarioTag);
        return written > 0 && static_cast<std::size_t>(written) < text.size()
                   ? std::string(text.data(), static_cast<std::size_t>(written))
                   : std::string("Source");
    }
    case HierarchyMode::activity: {
        const int written = snapshot.activitySession == 0
                                ? std::snprintf(text.data(), text.size(), "Activity / unresolved")
                                : std::snprintf(text.data(),
                                                text.size(),
                                                "Activity / 0x%016llX",
                                                static_cast<unsigned long long>(
                                                    snapshot.activitySession));
        return written > 0 && static_cast<std::size_t>(written) < text.size()
                   ? std::string(text.data(), static_cast<std::size_t>(written))
                   : std::string("Activity");
    }
    }
    return "World";
}

[[nodiscard]] FilterGroup filter_group(model::NodeKind kind) noexcept {
    switch (kind) {
    case model::NodeKind::geometry:
        return FilterGroup::geometry;
    case model::NodeKind::runtimeEntity:
    case model::NodeKind::physics:
        return FilterGroup::entities;
    case model::NodeKind::spawnGroup:
    case model::NodeKind::spawnSet:
    case model::NodeKind::spawnPoint:
        return FilterGroup::spawns;
    case model::NodeKind::activityLogic:
    case model::NodeKind::logicGroup:
    case model::NodeKind::logicEntity:
    case model::NodeKind::logicPlacement:
        return FilterGroup::logic;
    case model::NodeKind::trigger:
        return FilterGroup::triggers;
    case model::NodeKind::audio:
        return FilterGroup::audio;
    case model::NodeKind::light:
    case model::NodeKind::terrain:
        return FilterGroup::rendering;
    case model::NodeKind::navigation:
        return FilterGroup::navigation;
    default:
        return FilterGroup::entities;
    }
}

[[nodiscard]] bool is_structural(model::NodeKind kind) noexcept {
    return kind == model::NodeKind::world || kind == model::NodeKind::source
           || kind == model::NodeKind::activity || kind == model::NodeKind::activityGraph
           || kind == model::NodeKind::activityGraphNode
           || kind == model::NodeKind::activityReference
           || kind == model::NodeKind::destination || kind == model::NodeKind::unresolved;
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
    case FilterGroup::rendering:
        return g_state.showRendering;
    case FilterGroup::navigation:
        return g_state.showNavigation;
    }
    return true;
}

[[nodiscard]] bool filter_present(FilterGroup group) noexcept {
    return std::ranges::any_of(world().graph.nodes(), [group](const model::Node& node) {
        return node.producer != model::Producer::activityCatalog
               && !is_structural(node.kind) && filter_group(node.kind) == group;
    });
}

[[nodiscard]] bool node_admitted_by_filters(const model::Node& node,
                                            const model::Query& query) noexcept {
    // Activity graphs are a separate browse dataset. Keeping them out of live admission prevents
    // hundreds of static catalog rows from overwhelming the Scene Tree and ownership graph.
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

void admit_with_ancestors(const model::Graph& graph,
                          model::NodeId id,
                          std::unordered_set<std::uint64_t>& admitted) {
    std::size_t guard = 0;
    while (id && guard++ <= graph.nodes().size()) {
        if (!admitted.insert(id.value).second) {
            return;
        }
        const model::Node* node = graph.node(id);
        if (node == nullptr) {
            return;
        }
        id = node->parent;
    }
}

[[nodiscard]] std::string row_label(const model::Node& node,
                                    HierarchyMode mode,
                                    const provider::WorldSnapshot& snapshot) {
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

[[nodiscard]] model::NodeId hierarchy_root(const provider::WorldSnapshot& snapshot) noexcept {
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
    g_state.graphAdmitted.clear();
    if (g_state.graphScope == GraphScope::filteredHierarchy) {
        g_state.graphAdmitted = g_state.admitted;
        return hierarchy_root(world());
    }

    const model::NodeId selectedId = g_state.selection.selected();
    const model::Node* selected = world().graph.node(selectedId);
    if (selected == nullptr || selected->producer == model::Producer::activityCatalog
        || !g_state.admitted.contains(selectedId.value)) {
        g_state.graphAdmitted = g_state.admitted;
        return hierarchy_root(world());
    }

    const auto admit = [](model::NodeId id) {
        if (id && g_state.admitted.contains(id.value)) {
            g_state.graphAdmitted.insert(id.value);
        }
    };
    model::NodeId root = selectedId;
    if (selected->parent && g_state.admitted.contains(selected->parent.value)) {
        root = selected->parent;
    }
    admit(root);
    if (const model::Node* rootNode = world().graph.node(root); rootNode != nullptr) {
        for (const model::NodeId child : rootNode->children) {
            admit(child);
        }
    }
    admit(selectedId);
    for (const model::NodeId child : selected->children) {
        admit(child);
        if (const model::Node* childNode = world().graph.node(child); childNode != nullptr) {
            for (const model::NodeId grandchild : childNode->children) {
                admit(grandchild);
            }
        }
    }
    return root;
}

void append_rows(const model::Graph& graph,
                 model::NodeId id,
                 const std::unordered_set<std::uint64_t>& admitted,
                 bool searching,
                 std::uint8_t depth,
                 const provider::WorldSnapshot& snapshot) {
    if (!id || !admitted.contains(id.value)) {
        return;
    }
    const model::Node* node = graph.node(id);
    if (node == nullptr) {
        return;
    }

    bool hasChildren = false;
    for (const model::NodeId child : node->children) {
        if (admitted.contains(child.value)) {
            hasChildren = true;
            break;
        }
    }
    g_state.rows.push_back(
        TreeRow{id, row_label(*node, g_state.hierarchyMode, snapshot), depth, hasChildren});

    if (!searching && g_state.collapsed.contains(id.value)) {
        return;
    }
    const std::uint8_t childDepth =
        depth == (std::numeric_limits<std::uint8_t>::max)() ? depth
                                                            : static_cast<std::uint8_t>(depth + 1U);
    for (const model::NodeId child : node->children) {
        append_rows(graph, child, admitted, searching, childDepth, snapshot);
    }
}

void rebuild_rows() {
    const provider::WorldSnapshot& snapshot = world();
    const std::string queryText(g_state.search.data());
    if (g_state.rowsValid && g_state.cachedGeneration == snapshot.graph.generation()
        && g_state.cachedSearch == queryText && g_state.cachedMode == g_state.hierarchyMode
        && g_state.cachedHiddenRevision == g_state.hiddenRevision) {
        return;
    }

    g_state.rows.clear();
    g_state.admitted.clear();
    const model::Query query = model::parse_query(queryText);
    const bool searching = !query.terms.empty();
    g_state.admitted.reserve(snapshot.graph.nodes().size());

    for (const model::Node& node : snapshot.graph.nodes()) {
        if (node_admitted_by_filters(node, query)) {
            admit_with_ancestors(snapshot.graph, node.id, g_state.admitted);
        }
    }

    append_rows(snapshot.graph,
                hierarchy_root(snapshot),
                g_state.admitted,
                searching,
                0,
                snapshot);
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

[[nodiscard]] bool chip(const char* label,
                        bool active,
                        bool enabled,
                        const char* unavailableTooltip) noexcept {
    return control_button(label,
                          {0.0F, scaled(kControlHeight)},
                          active,
                          enabled,
                          unavailableTooltip);
}

[[nodiscard]] bool tab_button(const char* label, bool active, float width) noexcept {
    return control_button(label,
                          {width, scaled(kControlHeight)},
                          active,
                          true);
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
    const auto filterChip = [](const char* label,
                               FilterGroup group,
                               bool& value) noexcept {
        const bool present = filter_present(group);
        if (chip(label, value, present, "No matching objects are present in this snapshot.")) {
            value = !value;
            g_state.rowsValid = false;
        }
    };

    filterChip("Geometry", FilterGroup::geometry, g_state.showGeometry);
    ImGui::SameLine();
    filterChip("Entities", FilterGroup::entities, g_state.showEntities);
    ImGui::SameLine();
    filterChip("Spawns", FilterGroup::spawns, g_state.showSpawns);
    ImGui::SameLine();
    filterChip("Logic", FilterGroup::logic, g_state.showLogic);

    filterChip("Triggers", FilterGroup::triggers, g_state.showTriggers);
    ImGui::SameLine();
    filterChip("Audio", FilterGroup::audio, g_state.showAudio);
    ImGui::SameLine();
    filterChip("Rendering", FilterGroup::rendering, g_state.showRendering);
    ImGui::SameLine();
    filterChip("Navigation", FilterGroup::navigation, g_state.showNavigation);
    ImGui::SameLine();
    if (chip("Hidden", g_state.showHidden, true, "")) {
        g_state.showHidden = !g_state.showHidden;
        g_state.rowsValid = false;
    }
    ImGui::SameLine();
    if (chip("Errors", g_state.errorsOnly, true, "")) {
        g_state.errorsOnly = !g_state.errorsOnly;
        g_state.rowsValid = false;
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
        if (collapsed) {
            g_state.collapsed.erase(row.id.value);
        } else {
            g_state.collapsed.insert(row.id.value);
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

    const float rowHeight = padded_row_height(ImGui::GetTextLineHeight(),
                                              scaled(kInspectorRowPadding),
                                              scaled(kMinimumTreeRowHeight));
    bool scrollAdjusted = false;
    if (g_state.revealSelection && g_state.selection.selected()) {
        const auto iterator = std::find_if(g_state.rows.begin(),
                                           g_state.rows.end(),
                                           [](const TreeRow& row) {
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
    } else if (g_state.restoreTreeScroll && g_state.treeAnchor.valid) {
        const auto iterator =
            std::ranges::find_if(g_state.rows, [](const TreeRow& row) {
                const model::Node* node = world().graph.node(row.id);
                return node != nullptr && identity_matches(g_state.treeAnchor, *node);
            });
        if (iterator != g_state.rows.end()) {
            const float index = static_cast<float>(iterator - g_state.rows.begin());
            ImGui::SetScrollY(snap_scroll(index * rowHeight,
                                          rowHeight,
                                          ImGui::GetScrollMaxY()));
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
                drawList->AddLine({x, rowStart.y},
                                  {x, rowStart.y + rowHeight},
                                  kGuideColor);
            }
            ImGui::SetCursorScreenPos({rowStart.x + indent, rowStart.y});
            draw_disclosure(row, {rowStart.x + indent, rowStart.y}, rowHeight);
            ImGui::SameLine(0.0F, 1.0F);

            const bool selected = g_state.selection.selected() == row.id;
            const model::Node* node = world().graph.node(row.id);
            const bool helper = node != nullptr
                                && model::supports(node->actions, model::Action::hide);
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
        const std::size_t anchorIndex = (std::min)(
            static_cast<std::size_t>((std::max)(0.0F, scrollY) / rowHeight),
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
    ImGui::Separator();

    if (control_button("Expand all", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.collapsed.clear();
        g_state.rowsValid = false;
    }
    ImGui::SameLine();
    if (control_button("Collapse all", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.collapsed.clear();
        for (const model::Node& node : world().graph.nodes()) {
            if (!node.children.empty()) {
                g_state.collapsed.insert(node.id.value);
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

[[nodiscard]] bool inspector_header(const char* label,
                                    ImGuiTreeNodeFlags flags) noexcept {
    const ImVec2 padding{ImGui::GetStyle().FramePadding.x, scaled(kInspectorRowPadding)};
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
    const float width = height + ImGui::GetStyle().ItemInnerSpacing.x
                        + ImGui::CalcTextSize(label).x;
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
    const float contentHeight =
        (std::max)({ImGui::GetTextLineHeight(), fieldHeight, valueHeight});
    const float padding = ImGui::GetStyle().CellPadding.y;
    const ImVec2 rowMinimum{contentMinimum.x, contentMinimum.y - padding};
    const ImVec2 rowMaximum{contentMinimum.x,
                            contentMinimum.y + contentHeight + padding};
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
    (void)std::snprintf(text.data(),
                        text.size(),
                        "0x%0*llX",
                        width,
                        static_cast<unsigned long long>(value));
    property_row(name, text.data());
}

void property_i32(const char* name, std::int32_t value) noexcept {
    std::array<char, 24> text{};
    (void)std::snprintf(text.data(), text.size(), "%d", value);
    property_row(name, text.data());
}

[[nodiscard]] bool begin_properties(const char* id) noexcept {
    const ImVec2 cellPadding{ImGui::GetStyle().CellPadding.x,
                             scaled(kInspectorRowPadding)};
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
    if (!ImGui::BeginTable(id,
                           2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
                               | ImGuiTableFlags_BordersInnerH)) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, scaled(112.0F));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void end_properties() noexcept {
    ImGui::EndTable();
    ImGui::PopStyleVar();
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
                 node.boundsProvenance.has_value()
                     ? model::provenance_name(*node.boundsProvenance)
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
    property_u64("State hash", metadata.stateHash, 8);
    property_u64("Style hash", metadata.styleHash, 8);
    std::array<char, 96> position{};
    std::snprintf(position.data(),
                  position.size(),
                  "%.3f, %.3f",
                  static_cast<double>(metadata.authoredPosition[0]),
                  static_cast<double>(metadata.authoredPosition[1]));
    property_row("Authored position", position.data());
    property_u64("Catalog build", metadata.catalogBuild, 8);
    property_row("Catalog version", metadata.catalogVersion.c_str());
    property_row("Compatibility", metadata.buildMatch ? "Target build" : "Browse only");
    property_i32("Location releases", static_cast<std::int32_t>(metadata.releaseCount));
    property_i32("Activity references", static_cast<std::int32_t>(metadata.referenceCount));
    end_properties();

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
            std::snprintf(rotation.data(), rotation.size(), "%.4f, %.4f, %.4f, %.4f",
                          static_cast<double>(metadata.authoredRotation[0]),
                          static_cast<double>(metadata.authoredRotation[1]),
                          static_cast<double>(metadata.authoredRotation[2]),
                          static_cast<double>(metadata.authoredRotation[3]));
            property_row("Authored quaternion", rotation.data());
        }
        end_properties();
    }
    if ((metadata.roleName.find("Trigger") != std::string::npos
         || metadata.roleName.find("Spatial") != std::string::npos)
        && !node.transform.has_value()) {
        inspector_text("Static archive identifies spatial/volume logic, but this definition has no proven shape or world transform.", true);
    }
    if (metadata.linkedDefinitionTags.empty()) {
        return;
    }
    ImGui::TextUnformatted("Serialized definition links");
    for (const std::uint32_t linked : metadata.linkedDefinitionTags) {
        std::array<char, 64> label{};
        std::snprintf(label.data(), label.size(), "Open 0x%08X", linked);
        ImGui::PushID(static_cast<int>(linked));
        const model::NodeId target = logic_node_for_definition(linked);
        ImGui::BeginDisabled(!target);
        if (ImGui::Button(label.data())) {
            select_node(target);
        }
        ImGui::EndDisabled();
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

void draw_inspector_actions(const model::Node& node) noexcept {
    const camera::Status status = camera::status();
    bool sameLine = false;

    const auto button = [&sameLine](const char* label) noexcept {
        const float buttonWidth =
            ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F;
        if (sameLine) {
            const float contentRight =
                ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            const float nextRight = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x
                                    + buttonWidth;
            if (nextRight <= contentRight) {
                ImGui::SameLine();
            }
        }
        sameLine = true;
        const float buttonHeight = scaled(kControlHeight);
        if (!next_item_fully_visible(buttonHeight)) {
            ImGui::Dummy({buttonWidth, buttonHeight});
            return false;
        }
        return ImGui::Button(label, {buttonWidth, buttonHeight});
    };

    if (model::supports(node.actions, model::Action::focus)) {
        ImGui::BeginDisabled(!status.active
                             || (!node.transform.has_value()
                                 && !(node.bounds.has_value()
                                      && model::bounds_valid(*node.bounds))));
        if (button("Focus")) {
            focus(node.id);
        }
        ImGui::EndDisabled();
    }
    if (model::supports(node.actions, model::Action::hide) && button(hidden(node.id) ? "Show" : "Hide")) {
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
    if (button("Copy breadcrumb")) {
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
    section::header("Inspector");
    const model::Node* node = selected_node();
    if (node == nullptr) {
        ImGui::Separator();
        ImGui::TextDisabled("Select an object in the Scene Tree or viewport.");
        return;
    }

    const std::string breadcrumb = world().graph.breadcrumb(node->id);
    inspector_text(breadcrumb.c_str(), true);
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

    ImGui::TextUnformatted("Graph Relationships");
    ImGui::TextDisabled(
        "These are inspection-graph relationships. Raw serialized reference edges are not retained "
        "by the current provider.");
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
    return std::string_view(origin) == "runtime" ? kSpawn
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
    (void)std::snprintf(text.data(),
                        text.size(),
                        "0x%0*llX",
                        width,
                        static_cast<unsigned long long>(value));
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

void data_vec3(const char* field,
               const std::array<float, 3>& value,
               const char* origin) noexcept {
    std::array<char, 96> text{};
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(value[0]),
                        static_cast<double>(value[1]),
                        static_cast<double>(value[2]));
    data_row(field, "vec3", text.data(), origin);
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
    data_u64("native_key",
             "u64",
             capture::stable_native_key(world(), *node),
             16,
             "derived");
    const std::string stableIdentity = capture::stable_identity(world(), *node);
    data_row("stable_identity", "string", stableIdentity.c_str(), "derived");
    if (node->parent) {
        data_u64("parent_id", "u64", node->parent.value, 16, "derived");
    }
    data_row("name", "string", node->name.c_str(), "derived");
    data_row("kind", "enum", model::kind_name(node->kind), "derived");
    data_row("status", "enum", model::status_name(node->status), "derived");
    data_i32("child_count",
             "u32",
             static_cast<std::int32_t>(node->children.size()),
             "derived");

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
        const char* const boundsOrigin =
            node->boundsProvenance.has_value()
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

    ImGui::EndTable();
    ImGui::TextDisabled(
        "Raw package offsets and encoded sizes are unavailable in the current reduced catalog.");
}

void draw_diagnostics() noexcept {
    const auto& diagnostics = world().diagnostics;
    if (diagnostics.empty()) {
        ImGui::TextDisabled("No inspector diagnostics for this world snapshot.");
        return;
    }

    ImGui::TextDisabled("%zu diagnostics", diagnostics.size());
    ImGui::SameLine();
    if (control_button("Copy diagnostics",
                       {0.0F, scaled(kControlHeight)},
                       false,
                       true)) {
        std::string report;
        for (const model::Diagnostic& diagnostic : diagnostics) {
            const char* prefix = diagnostic.severity == model::Diagnostic::Severity::error
                                     ? "ERROR"
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
    if (g_state.lastExport.success) {
        ImGui::TextWrapped("Exported: %ls", g_state.lastExport.path.data());
    } else if (g_state.lastExport.error[0] != '\0') {
        ImGui::TextColored(kFailure, "Export failed: %s", g_state.lastExport.error.data());
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
            ImGui::SetTooltip("%s\nprovenance: %s", event.identity.c_str(),
                              event.provenance.c_str());
        }
    }
}

void draw_events() noexcept {
    const bool recording = g_state.history.recording();
    if (control_button(recording ? "Pause tracking" : "Start tracking",
                       {0.0F, scaled(kControlHeight)}, recording, true)) {
        g_state.history.set_recording(!recording, world());
    }
    ImGui::SameLine();
    if (control_button("Clear", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.history.clear();
    }
    ImGui::SameLine();
    if (control_button("Export events", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.lastExport = capture::export_events(g_state.history.events(),
                                                     capture::make_snapshot(world()));
    }
    ImGui::SameLine();
    if (control_button("Snapshot JSON", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.lastExport = capture::export_json(capture::make_snapshot(world()));
    }
    ImGui::SameLine();
    if (control_button("Node CSV", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.lastExport = capture::export_csv(capture::make_snapshot(world()));
    }
    bool optionsChanged = false;
    optionsChanged = ImGui::Checkbox("Runtime only", &g_state.trackRuntimeOnly) || optionsChanged;
    ImGui::SameLine();
    optionsChanged = ImGui::Checkbox("Track transforms", &g_state.trackTransforms) || optionsChanged;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Transform changes use a 5 cm position threshold to suppress frame noise.");
    }
    if (optionsChanged) {
        g_state.history.set_options(
            capture::ChangeTrackingOptions{g_state.trackRuntimeOnly, g_state.trackTransforms, 0.05F},
            world());
    }
    ImGui::SetNextItemWidth((std::max)(180.0F, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##event_filter", "Filter node, kind, identity, field, or value",
                             g_state.eventFilter.data(), g_state.eventFilter.size());
    draw_export_status();
    ImGui::Separator();
    ImGui::TextDisabled("%zu / %zu events", g_state.history.events().size(),
                        capture::kEventCapacity);
    draw_event_rows(g_state.history.events());
}

void draw_compare() noexcept {
    if (control_button("Set baseline", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.comparisonBaseline = capture::make_snapshot(world());
        g_state.comparisonEvents.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_state.comparisonBaseline.has_value());
    if (control_button("Compare now", {0.0F, scaled(kControlHeight)}, false, true)) {
        g_state.comparisonEvents = capture::compare(
            *g_state.comparisonBaseline, capture::make_snapshot(world()));
    }
    ImGui::EndDisabled();
    if (!g_state.comparisonBaseline.has_value()) {
        ImGui::TextDisabled("Capture a baseline to compare complete pointer-free snapshots.");
        return;
    }
    ImGui::TextDisabled("Baseline tick: %llu, changes: %zu",
                        static_cast<unsigned long long>(g_state.comparisonBaseline->capturedTick),
                        g_state.comparisonEvents.size());
    draw_event_rows(g_state.comparisonEvents);
}

void draw_bottom_dock() noexcept {
    const float contentWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const char* hide = "Hide";
    const float hideWidth =
        ImGui::CalcTextSize(hide).x + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float headerGap = scaled(8.0F);
    const float minimumTabWidth = scaled(76.0F);
    const bool compactHeader =
        contentWidth < hideWidth + headerGap + minimumTabWidth * 5.0F + spacing * 4.0F;
    const float tabArea = compactHeader ? contentWidth
                                        : (std::max)(1.0F, contentWidth - hideWidth - headerGap);
    const float tabWidth = (std::max)(1.0F, (tabArea - spacing * 4.0F) / 5.0F);

    const auto tab = [tabWidth](const char* label, BottomTab tabValue) noexcept {
        if (tab_button(label, g_state.bottomTab == tabValue, tabWidth)) {
            g_state.bottomTab = tabValue;
        }
    };

    tab("References", BottomTab::references);
    ImGui::SameLine();
    tab("Data", BottomTab::data);
    ImGui::SameLine();
    tab("Changes", BottomTab::events);
    ImGui::SameLine();
    tab("Compare", BottomTab::compare);
    ImGui::SameLine();
    tab("Diagnostics", BottomTab::diagnostics);

    if (compactHeader) {
        if (control_button("Hide bottom panel",
                           {ImGui::GetContentRegionAvail().x, scaled(kControlHeight)},
                           false,
                           true)) {
            g_state.bottomCollapsed = true;
            persist_layout();
        }
    } else {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - hideWidth);
        if (control_button(hide,
                           {hideWidth, scaled(kControlHeight)},
                           false,
                           true)) {
            g_state.bottomCollapsed = true;
            persist_layout();
        }
    }

    ImGui::Separator();
    const float contentHeight = (std::max)(0.0F, ImGui::GetContentRegionAvail().y);
    const ImGuiWindowFlags contentFlags = g_state.bottomTab == BottomTab::data
                                              ? ImGuiWindowFlags_NoScrollbar
                                              : ImGuiWindowFlags_None;
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
    if (model::supports(node->actions, model::Action::focus)) {
        ImGui::BeginDisabled(!status.active
                             || (!node->transform.has_value()
                                 && !(node->bounds.has_value()
                                      && model::bounds_valid(*node->bounds))));
        if (ImGui::MenuItem("Focus", "F")) {
            focus(node->id);
        }
        ImGui::EndDisabled();
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
    if (node->transform.has_value()
        && model::supports(node->actions, model::Action::copyPosition)
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
        g_state.selection.clear();
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
    if (control_button("Previous graph", {0.0F, scaled(kControlHeight)}, false, graphIndex != 0)) {
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
    if (control_button("Next graph", {0.0F, scaled(kControlHeight)}, false, hasNext)) {
        open_activity_graph(graphs[graphIndex + 1]);
        ++graphIndex;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s / build %u / target %u / %s",
                        world().activityCatalogVersion.c_str(),
                        world().activityCatalogBuild,
                        activity_catalog::kTargetContentBuild,
                        world().activityCatalogBuildMatch ? "Target build" : "Browse only");
    ImGui::TextDisabled("Authored positions only; each canvas is one graph coordinate space. No node connections are present in this catalog.");
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
        const float x = spanX > 0.0F
                            ? (node.activityMetadata->authoredPosition[0] - minimumX) / spanX
                            : 0.5F;
        const float y = spanY > 0.0F
                            ? (node.activityMetadata->authoredPosition[1] - minimumY) / spanY
                            : 0.5F;
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
        if (coincidentGraph != g_state.selectedActivityGraphHash || coincidentPosition != position) {
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
        drawList->AddRectFilled(card.minimum,
                                card.maximum,
                                selected ? IM_COL32(54, 48, 35, 250)
                                         : (isHovered ? IM_COL32(42, 44, 56, 250)
                                                      : IM_COL32(25, 27, 35, 245)),
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
        ImGui::TextDisabled("Repeated clicks cycle the coincident nodes without moving their authored coordinates.");
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
void draw_toolbar(const camera::Status& status) noexcept {
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
            world().packageName.empty() ? "No world" : world().packageName.c_str();
        ImGui::TextUnformatted(package);
        if (world().bubble.has_value()) {
            ImGui::SameLine();
            ImGui::TextDisabled("/ Bubble %u", static_cast<unsigned>(*world().bubble));
        }
        if (world().activityIndex >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("/ Activity %d", world().activityIndex);
        }

        ImGui::Separator();
        if (ImGui::MenuItem(status.requested ? "Viewer: On" : "Viewer: Off")) {
            camera::request_active(!status.requested);
        }
        if (ImGui::BeginMenu("Camera Path")) {
            static std::size_t selectedPath = 0;
            camera_paths::Library library = camera_paths::get();
            if (selectedPath >= library.paths.size()) {
                selectedPath = 0;
            }
            for (std::size_t index = 0; index < library.paths.size(); ++index) {
                if (ImGui::MenuItem(library.paths[index].name.c_str(), nullptr,
                                    selectedPath == index)) {
                    selectedPath = index;
                }
            }
            ImGui::Separator();
            const bool pathPresent = selectedPath < library.paths.size();
            ImGui::BeginDisabled(!status.active || !pathPresent
                                 || library.paths[selectedPath].keyframes.empty());
            if (ImGui::MenuItem("Play")) {
                const camera_paths::CameraPath& stored = library.paths[selectedPath];
                camera::PlaybackPath path{};
                path.keyframeCount = (std::min)(stored.keyframes.size(), path.keyframes.size());
                path.loop = stored.loop;
                std::snprintf(path.name.data(), path.name.size(), "%s", stored.name.c_str());
                path.activitySession = world().activitySession;
                path.activityRevision = world().activityRevision;
                for (std::size_t index = 0; index < path.keyframeCount; ++index) {
                    const camera_paths::Keyframe& source = stored.keyframes[index];
                    path.keyframes[index].pose.position = source.position;
                    path.keyframes[index].pose.yaw = source.yaw;
                    path.keyframes[index].pose.pitch = source.pitch;
                    path.keyframes[index].pose.fov = source.fov;
                    std::snprintf(path.keyframes[index].label.data(),
                                  path.keyframes[index].label.size(), "%s", source.label.c_str());
                    path.keyframes[index].travelSeconds = source.travelSeconds;
                    path.keyframes[index].dwellSeconds = source.dwellSeconds;
                    path.keyframes[index].captureSnapshot = source.captureSnapshot;
                }
                (void)camera::request_playback(path);
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!status.active || !pathPresent
                                 || library.paths[selectedPath].keyframes.size()
                                        >= camera_paths::kMaximumKeyframeCount);
            if (ImGui::MenuItem("Add keyframe from current camera")) {
                camera_paths::Keyframe keyframe{};
                keyframe.position = status.pose.position;
                keyframe.yaw = status.pose.yaw;
                keyframe.pitch = status.pose.pitch;
                keyframe.fov = std::clamp(status.pose.fov, camera_paths::kMinimumKeyframeFov,
                                          camera_paths::kMaximumKeyframeFov);
                keyframe.label = "Keyframe "
                                 + std::to_string(library.paths[selectedPath].keyframes.size() + 1);
                const model::Node* selected = selected_node();
                if (selected != nullptr) {
                    keyframe.selection = camera_paths::SelectionIdentity{
                        world().producerEpoch,
                        capture::stable_native_key(world(), *selected),
                        static_cast<std::uint32_t>(selected->producer),
                        static_cast<std::uint32_t>(selected->kind)};
                }
                library.paths[selectedPath].keyframes.push_back(std::move(keyframe));
                (void)camera_paths::publish(library);
            }
            ImGui::EndDisabled();
            const camera::PlaybackStatus playback = camera::playback_status();
            ImGui::BeginDisabled(!playback.playing);
            if (ImGui::MenuItem(playback.paused ? "Resume" : "Pause")) {
                camera::request_playback_pause(!playback.paused);
            }
            if (ImGui::MenuItem("Stop")) {
                camera::request_playback_stop();
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Center view")) {
            if (ImGui::MenuItem("World", nullptr, g_state.centerMode == CenterMode::world)) {
                g_state.centerMode = CenterMode::world;
            }
            if (ImGui::MenuItem("Node Graph",
                                nullptr,
                                g_state.centerMode == CenterMode::nodeGraph)) {
                g_state.centerMode = CenterMode::nodeGraph;
                g_state.graphState.fitRequested = true;
            }
            if (ImGui::MenuItem("Activity Map",
                                nullptr,
                                g_state.centerMode == CenterMode::activityMap)) {
                g_state.centerMode = CenterMode::activityMap;
            }
            ImGui::Separator();
            ImGui::BeginDisabled(g_state.centerMode != CenterMode::nodeGraph);
            if (ImGui::MenuItem("Fit Node Graph")) {
                g_state.graphState.fitRequested = true;
            }
            if (ImGui::MenuItem("Center selected node")) {
                g_state.graphState.centerRequested = g_state.selection.selected();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Selected neighborhood",
                                nullptr,
                                g_state.graphScope == GraphScope::selectionNeighborhood)) {
                g_state.graphScope = GraphScope::selectionNeighborhood;
                g_state.graphState.fitRequested = true;
            }
            if (ImGui::MenuItem("All filtered nodes",
                                nullptr,
                                g_state.graphScope == GraphScope::filteredHierarchy)) {
                g_state.graphScope = GraphScope::filteredHierarchy;
                g_state.graphState.fitRequested = true;
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Overlays")) {
            const InspectorCapabilities available = capabilities();
            if (ImGui::MenuItem("Spawn helpers", nullptr, &g_state.showSpawns)) {
                g_state.rowsValid = false;
            }
            ImGui::BeginDisabled(available.logicPlacements == 0);
            if (ImGui::MenuItem("Authored activity placements", nullptr, &g_state.showLogic)) {
                g_state.rowsValid = false;
            }
            ImGui::EndDisabled();
            if (available.logicPlacements == 0
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("No exact WorldID-backed activity-logic placements are present for this scenario.");
            }
            ImGui::BeginDisabled(available.knownBounds == 0);
            ImGui::MenuItem("Known bounds (x-ray)", nullptr, &g_state.showKnownBounds);
            ImGui::EndDisabled();
            if (available.knownBounds == 0
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("No current producer supplies a validated AABB. The overlay never guesses bounds.");
            }
            ImGui::BeginDisabled(available.triggerCenters == 0);
            ImGui::MenuItem("Trigger centers", nullptr, &g_state.showTriggerCenters);
            ImGui::EndDisabled();
            if (available.triggerCenters == 0
                && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("No trigger center observations are present in this snapshot.");
            }
            ImGui::MenuItem("Helper labels", nullptr, &g_state.showLabels);
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
                                  : (hovered ? ImGuiCol_SeparatorHovered
                                             : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                              ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(color),
                                              scaled(2.0F));
    if (hovered || active) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    if (active && maximum >= minimum) {
        const float delta =
            vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
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
    const InspectorCapabilities available = capabilities();
    ImGui::SameLine();
    ImGui::TextDisabled("| %.0f FPS", static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::SameLine();
    ImGui::TextDisabled("| %zu nodes | %zu spatial | %zu bounds",
                        available.liveNodes,
                        available.spatialNodes,
                        available.knownBounds);
    if (available.logicDefinitions != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("| Logic %zu / %zu placed",
                            available.logicDefinitions,
                            available.logicPlacements);
    }
    if (available.triggerCenters != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("| triggers %zu centers / %zu shapes",
                            available.triggerCenters,
                            available.triggerShapes);
    }
    if (available.errors != 0 || available.warnings != 0) {
        ImGui::SameLine();
        ImGui::TextColored(available.errors != 0 ? kFailure : kWarning,
                           "| %zu errors / %zu warnings",
                           available.errors,
                           available.warnings);
    }
    if (world().activityCatalogPresent && !world().activityCatalogBuildMatch) {
        ImGui::SameLine();
        ImGui::TextColored(kWarning, "| catalog browse-only");
    }
    if (g_state.bottomCollapsed) {
        ImGui::SameLine();
        if (control_button("Show bottom", {0.0F, scaled(kControlHeight)}, false, true)) {
            g_state.bottomCollapsed = false;
            persist_layout();
        }
    }
    if (world().bubble.has_value()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| Bubble %u", static_cast<unsigned>(*world().bubble));
    }
    const model::Node* selected = selected_node();
    if (selected != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", model::kind_name(selected->kind));
    }

    const char* readOnly = "READ ONLY";
    const float width = ImGui::CalcTextSize(readOnly).x;
    ImGui::SameLine(
        (std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width - scaled(10.0F)));
    ImGui::TextColored(kSelection, "%s", readOnly);
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
    ImGui::PushStyleColor(ImGuiCol_Header,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.18F));
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
    g_state = {};
    g_open.store(false, std::memory_order_release);
    viewer_input::reset();
}

void shutdown() noexcept {
    viewer_input::reset();
    g_open.store(false, std::memory_order_release);
    g_state.provider.reset();
    g_state = {};
}

void open() noexcept {
    g_open.store(true, std::memory_order_release);
}

void close() noexcept {
    g_open.store(false, std::memory_order_release);
    suspend();
}

void suspend() noexcept {
    g_state.viewportNavigation = false;
    viewer_input::reset();
}

bool visible() noexcept {
    return g_open.load(std::memory_order_acquire);
}

bool render(bool uiVisible) noexcept {
    if (!visible()) {
        g_state.viewportNavigation = false;
        viewer_input::reset();
        return false;
    }
    if (!uiVisible) {
        g_state.viewportNavigation = false;
        viewer_input::reset();
        return false;
    }

    initialize_layout();
    refresh_layout_scale();
    const std::uint32_t previousGeneration = world().graph.generation();
    const model::NodeId previousObservationStart = first_observation_node(world());
    const model::NodeId previousSelection = g_state.selection.selected();
    const NodeIdentity previousSelectionIdentity =
        node_identity(world().graph.node(previousSelection));
    const bool rebuilt = g_state.provider.refresh();
    g_state.history.observe(world());
    if (rebuilt) {
        if (world().graph.generation() != previousGeneration) {
            g_state.hidden.clear();
            g_state.collapsed.clear();
            g_state.hiddenRevision = 0;
        } else if (previousObservationStart) {
            const auto observationState = [previousObservationStart](std::uint64_t id) noexcept {
                return id >= previousObservationStart.value;
            };
            if (observationState(previousSelection.value)) {
                g_state.selection.clear();
                const model::NodeId replacement = find_node(previousSelectionIdentity);
                if (replacement) {
                    g_state.selection.select(replacement);
                }
            }
            if (std::erase_if(g_state.hidden, observationState) != 0) {
                ++g_state.hiddenRevision;
            }
            std::erase_if(g_state.collapsed, observationState);
            g_state.restoreTreeScroll = true;
        }
        g_state.selection.reconcile(world().graph);
        g_state.rowsValid = false;
    }
    g_state.selection.clear_hover();

    const camera::Status cameraStatus = camera::status();
    const renderer::frame_capture::View capturedFrame = renderer::captured_frame_locked();
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr || mainViewport->Size.x <= 0.0F
        || mainViewport->Size.y <= 0.0F) {
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
        draw_toolbar(cameraStatus);

        const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
        const float statusHeight = scaled(kStatusHeight);
        const float splitterSize = scaled(kSplitterThickness);
        const float availableHeight = (std::max)(0.0F, ImGui::GetContentRegionAvail().y);
        const float availableWidth = (std::max)(1.0F, ImGui::GetContentRegionAvail().x);

        const VerticalLayout vertical = compute_vertical_layout(
            availableHeight,
            statusHeight,
            splitterSize,
            scaled(kMinimumMainHeight),
            g_state.bottomHeight,
            scaled(kMinimumBottomUsableHeight),
            scaled(client::viewer::kMinimumInspectorBottomHeight),
            g_state.bottomCollapsed);
        const bool bottomVisible = vertical.bottomVisible;
        const float minimumBottom = vertical.minimumBottom;
        const float maximumVisibleBottom = vertical.maximumBottom;
        const float bottomHeight = vertical.bottomHeight;
        const float mainHeight = vertical.mainHeight;

        const float minimumLeft = scaled(client::viewer::kMinimumInspectorLeftWidth);
        const float maximumLeft = scaled(client::viewer::kMaximumInspectorLeftWidth);
        const float minimumRight = scaled(client::viewer::kMinimumInspectorRightWidth);
        const float maximumRight = scaled(client::viewer::kMaximumInspectorRightWidth);
        const float minimumCenter = scaled(kMinimumViewportWidth);
        const float horizontalBudget =
            (std::max)(1.0F, availableWidth - splitterSize * 2.0F);

        float leftWidth = std::clamp(g_state.leftWidth, minimumLeft, maximumLeft);
        float rightWidth = std::clamp(g_state.rightWidth, minimumRight, maximumRight);
        const bool sideMinimumsFit =
            horizontalBudget >= minimumCenter + minimumLeft + minimumRight;
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

        float centerWidth =
            (std::max)(1.0F, horizontalBudget - leftWidth - rightWidth);

        ImGui::SetCursorScreenPos(contentOrigin);
        ImGui::BeginChild("##world_outliner",
                          {leftWidth, mainHeight},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_outliner();
        ImGui::EndChild();

        ImGui::SetCursorScreenPos({contentOrigin.x + leftWidth, contentOrigin.y});
        const float leftDragMaximum = sideMinimumsFit
                                          ? (std::min)(maximumLeft,
                                                       horizontalBudget - minimumCenter - rightWidth)
                                          : 0.0F;
        const SplitterResult leftResult = splitter("##left_splitter",
                                                   mainHeight,
                                                   true,
                                                   leftWidth,
                                                   1.0F,
                                                   minimumLeft,
                                                   leftDragMaximum);
        if (leftResult.changed) {
            g_state.leftWidth = leftWidth;
        }
        finish_splitter(leftResult);
        centerWidth = (std::max)(1.0F, horizontalBudget - leftWidth - rightWidth);

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
        if (worldCenter) {
            const viewport::Options overlayOptions{g_state.showGeometry,
                                                   g_state.showEntities,
                                                   g_state.showSpawns,
                                                   g_state.showLogic,
                                                   g_state.showTriggers,
                                                   g_state.showAudio,
                                                   g_state.showRendering,
                                                   g_state.showNavigation,
                                                   g_state.showLabels,
                                                   g_state.showKnownBounds,
                                                   g_state.showTriggerCenters};
            interaction = viewport::draw(world().graph,
                                         g_state.selection.selected(),
                                         g_state.hidden,
                                         g_state.admitted,
                                         cameraStatus,
                                         capturedFrame,
                                         overlayOptions,
                                         g_state.viewportNavigation);
        } else if (g_state.centerMode == CenterMode::nodeGraph) {
            const model::NodeId graphRoot = prepare_graph_admission();
            const std::uint64_t graphRevision =
                g_state.admissionRevision ^ g_state.selection.selected().value
                ^ (g_state.graphScope == GraphScope::selectionNeighborhood
                       ? 0x8000000000000000ULL
                       : 0ULL);
            const graph::Result graphInteraction =
                graph::draw(world().graph,
                            graphRoot,
                            g_state.selection.selected(),
                            g_state.graphAdmitted,
                            graphRevision,
                            g_state.graphState);
            interaction.hovered = graphInteraction.hovered;
            interaction.selected = graphInteraction.selected;
            interaction.focused = graphInteraction.focused;
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
            g_state.selection.clear();
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
        const float rightDragMaximum = sideMinimumsFit
                                           ? (std::min)(maximumRight,
                                                        horizontalBudget - minimumCenter - leftWidth)
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
            ImGui::SetCursorScreenPos(
                {contentOrigin.x, contentOrigin.y + vertical.splitterY});
            float draggedBottomHeight = bottomHeight;
            const SplitterResult bottomResult =
                splitter("##bottom_splitter",
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
                              ImGuiChildFlags_Borders
                                  | ImGuiChildFlags_AlwaysUseWindowPadding);
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
    return true;
}

bool selected_identity(SelectionIdentity& identity) noexcept {
    const model::Node* node = selected_node();
    if (node == nullptr) {
        identity = {};
        return false;
    }
    identity.producerEpoch = world().producerEpoch;
    identity.nativeKey = capture::stable_native_key(world(), *node);
    identity.producer = static_cast<std::uint32_t>(node->producer);
    identity.kind = static_cast<std::uint32_t>(node->kind);
    return identity.nativeKey != 0;
}

void service_camera_path_captures() noexcept {
    camera::SnapshotCaptureRequest request{};
    while (camera::consume_snapshot_capture_request(request)) {
        const std::uint32_t previousGeneration = world().graph.generation();
        const bool rebuilt = g_state.provider.refresh();
        if (rebuilt) {
            if (world().graph.generation() != previousGeneration) {
                g_state.hidden.clear();
                g_state.collapsed.clear();
                ++g_state.hiddenRevision;
            }
            g_state.selection.reconcile(world().graph);
            g_state.rowsValid = false;
        }
        g_state.history.observe(world());
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
        g_state.lastExport =
            capture::export_route_json(capture::make_snapshot(world()), metadata);
    }
}

} // namespace sunrise::client::ui::world_inspector
