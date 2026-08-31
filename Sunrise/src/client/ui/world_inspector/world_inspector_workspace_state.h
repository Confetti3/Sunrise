#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../inspection/inspection_session.h"
#include "../../inspection/inspection_settings_store.h"
#include "world_inspector_graph.h"
#include "world_inspector_viewport.h"

namespace sunrise::client::ui::world_inspector {

enum class CenterMode : std::uint8_t {
    overview,
    world,
    nodeGraph,
    relationships,
    activityMap,
};

enum class BottomTab : std::uint8_t {
    references,
    data,
    events,
    compare,
    diagnostics,
};

enum class HierarchyMode : std::uint8_t {
    world,
    source,
    activity,
};

enum class GraphScope : std::uint8_t {
    selectionNeighborhood,
    filteredHierarchy,
};

enum class FilterGroup : std::uint8_t {
    geometry,
    entities,
    spawns,
    logic,
    triggers,
    audio,
};

struct TreeRow final {
    inspection::NodeId id{};
    std::string label;
    std::uint8_t depth{};
    bool hasChildren{};
};

struct TreeTraversal final {
    inspection::NodeId id{};
    std::uint8_t depth{};
};

struct WorkspaceState final {
    static constexpr std::size_t searchCapacity = 256;

    inspection::InspectionSession session;
    inspection::Selection selection;
    inspection::NodeKey selectedKey{};
    std::unordered_set<std::uint64_t> hidden;
    std::unordered_set<std::uint64_t> collapsed;
    std::unordered_set<inspection::NodeKey, inspection::NodeKeyHash> hiddenKeys;
    std::unordered_set<inspection::NodeKey, inspection::NodeKeyHash> collapsedKeys;
    std::vector<TreeRow> rows;
    std::vector<TreeTraversal> rowTraversal;
    std::unordered_set<std::uint64_t> rowVisited;
    std::unordered_set<std::uint64_t> admitted;
    std::unordered_set<std::uint64_t> overviewAdmitted;
    std::unordered_set<std::uint64_t> graphAdmitted;
    std::uint64_t admissionRevision{};
    std::size_t graphOmitted{};
    graph::OverviewState overviewGraphState;
    graph::State ownershipGraphState;
    graph::State relationshipGraphState;
    std::array<char, searchCapacity> search{};
    std::array<char, searchCapacity> eventFilter{};
    std::array<char, searchCapacity> activitySearch{};
    CenterMode centerMode{CenterMode::world};
    GraphScope graphScope{GraphScope::selectionNeighborhood};
    std::uint32_t selectedActivityGraphHash{};
    std::string cachedSearch;
    std::string queryError;
    inspection::NodeId contextTarget{};
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
    bool stableStateInitialized{};
    bool rowsValid{};
    bool showGeometry{true};
    bool showEntities{true};
    bool showSpawns{true};
    bool showLogic{true};
    bool showTriggers{true};
    bool showAudio{true};
    bool showKnownBounds{true};
    bool showTriggerCenters{true};
    bool showAuthoredOrientation{true};
    bool showHidden{true};
    bool errorsOnly{};
    bool showLabels{};
    viewport::Detail overlayDetail{viewport::Detail::adaptive};
    std::uint32_t maximumVisibleNodes{inspection::settings::kDefaultMaximumVisibleNodes};
    float nearbyRadius{inspection::settings::kDefaultNearbyRadius};
    float glyphSizePixels{inspection::settings::kDefaultGlyphSizePixels};
    float lineWidthPixels{inspection::settings::kDefaultLineWidthPixels};
    float baseOpacity{inspection::settings::kDefaultBaseOpacity};
    float focusContextOpacity{inspection::settings::kDefaultFocusContextOpacity};
    bool trackRuntimeOnly{true};
    bool trackTransforms{};
    bool liveRuntimeMembership{};
    bool bottomCollapsed{};
    bool viewportNavigation{};
    bool layoutDirty{};
    bool contextRequested{};
    bool focusSearch{};
    bool activityBrowserOpen{};
    bool revealSelection{};
    inspection::NodeKey treeAnchor;
    float treeAnchorOffset{};
    bool restoreTreeScroll{};
    viewport::Result lastViewportResult{};
    std::chrono::steady_clock::time_point lastOverlayFailureLog{};
    std::chrono::steady_clock::time_point lastAllocatorLog{};
    std::size_t lastLoggedArenaMisses{};
    std::size_t lastLoggedAllocationFailures{};
};

} // namespace sunrise::client::ui::world_inspector
