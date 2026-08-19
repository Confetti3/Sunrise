#include "world_inspector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "../../../core/ui/modules/logs/logs.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../hooks/graphics/renderer/renderer.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../inspection/providers/spawn_inspection_provider.h"
#include "../../inspection/world_inspection_model.h"
#include "../../viewer/viewer_camera_settings_store.h"
#include "../../viewer/viewer_input_ownership.h"
#include "world_inspector_viewport.h"

namespace sunrise::client::ui::world_inspector {
namespace {

namespace camera = client::viewer::camera;
namespace clipboard = core::ui::modules::logs;
namespace dpi = core::ui::scaling::dpi;
namespace model = client::inspection;
namespace provider = client::inspection::providers;
namespace renderer = client::hooks::graphics::renderer;
namespace viewer_input = client::viewer::input;

constexpr float kToolbarHeight = 30.0F;
constexpr float kStatusHeight = 22.0F;
constexpr float kSplitterThickness = 4.0F;
constexpr float kMinimumViewportWidth = 360.0F;
constexpr float kTreeRowHeight = 24.0F;
constexpr float kTreeIndent = 14.0F;
constexpr std::size_t kSearchCapacity = 256;
constexpr ImU32 kGuideColor = IM_COL32(49, 57, 67, 210);
constexpr ImVec4 kSelection{0.259F, 0.722F, 0.906F, 1.0F};
constexpr ImVec4 kSpawn{0.902F, 0.722F, 0.290F, 1.0F};
constexpr ImVec4 kWarning{0.949F, 0.549F, 0.216F, 1.0F};
constexpr ImVec4 kFailure{0.941F, 0.349F, 0.349F, 1.0F};
constexpr ImVec4 kMuted{0.50F, 0.55F, 0.61F, 1.0F};

constexpr ImGuiWindowFlags kWorkspaceFlags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

enum class HierarchyMode : std::uint8_t {
    world,
    source,
    activity,
};

enum class BottomTab : std::uint8_t {
    references,
    data,
    diagnostics,
};

struct TreeRow final {
    model::NodeId id{};
    std::string label;
    std::uint8_t depth{};
    bool hasChildren{};
};

struct WorkspaceState final {
    provider::SpawnInspectionProvider provider;
    model::Selection selection;
    std::unordered_set<std::uint64_t> hidden;
    std::unordered_set<std::uint64_t> collapsed;
    std::vector<TreeRow> rows;
    std::array<char, kSearchCapacity> search{};
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
    bool showSpawns{true};
    bool showHidden{true};
    bool errorsOnly{};
    bool showLabels{};
    bool bottomCollapsed{};
    bool viewportNavigation{};
    bool layoutDirty{};
    bool contextRequested{};
};

std::atomic_bool g_open{};
WorkspaceState g_state{};

[[nodiscard]] float scaled(float value) noexcept {
    return dpi::pixels(value);
}

[[nodiscard]] const provider::WorldSnapshot& world() noexcept {
    return g_state.provider.snapshot();
}

[[nodiscard]] const model::Node* selected_node() noexcept {
    return world().graph.node(g_state.selection.selected());
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
        if (node.kind == model::NodeKind::spawnPoint && node.id != id) {
            g_state.hidden.insert(node.id.value);
        }
    }
    note_visibility_change();
}

void focus(model::NodeId id) noexcept {
    const model::Node* node = world().graph.node(id);
    const camera::Status status = camera::status();
    if (node == nullptr || !node->transform.has_value() || !status.active
        || !model::supports(node->actions, model::Action::focus)) {
        return;
    }
    camera::Pose pose = status.pose;
    constexpr float kFocusDistance = 6.0F;
    for (std::size_t lane = 0; lane < pose.position.size(); ++lane) {
        pose.position[lane] = node->transform->position[lane] - pose.forward[lane] * kFocusDistance;
    }
    (void)camera::move_to(pose);
}

void select_and_focus(model::NodeId id) noexcept {
    if (world().graph.node(id) != nullptr) {
        g_state.selection.select(id);
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

void rebuild_rows() {
    const provider::WorldSnapshot& snapshot = world();
    const std::string queryText(g_state.search.data());
    if (g_state.rowsValid && g_state.cachedGeneration == snapshot.graph.generation()
        && g_state.cachedSearch == queryText && g_state.cachedMode == g_state.hierarchyMode
        && g_state.cachedHiddenRevision == g_state.hiddenRevision) {
        return;
    }

    g_state.rows.clear();
    const model::Query query = model::parse_query(queryText);
    const bool searching = !query.terms.empty();
    const model::Node* root = snapshot.graph.node(snapshot.graph.root());
    const model::Node* set = snapshot.graph.node(snapshot.spawnSetNode);
    std::vector<model::NodeId> points;
    if (g_state.showSpawns && set != nullptr) {
        points.reserve(set->children.size());
        for (const model::NodeId id : set->children) {
            const model::Node* node = snapshot.graph.node(id);
            if (node == nullptr || node->kind != model::NodeKind::spawnPoint
                || (!g_state.showHidden && hidden(id))
                || (g_state.errorsOnly && node->status != model::Status::failed)
                || !model::matches(*node, query)) {
                continue;
            }
            points.push_back(id);
        }
    }

    const bool rootMatches = root != nullptr && model::matches(*root, query);
    const bool setMatches = set != nullptr && model::matches(*set, query);
    const bool showBranch = !searching || rootMatches || setMatches || !points.empty();
    if (root != nullptr && showBranch) {
        g_state.rows.push_back(
            TreeRow{root->id, root_label(g_state.hierarchyMode, snapshot), 0, set != nullptr});
    }
    const bool rootOpen = root != nullptr
                          && (searching || !g_state.collapsed.contains(root->id.value));
    if (set != nullptr && showBranch && rootOpen) {
        g_state.rows.push_back(TreeRow{set->id, set->name, 1, !set->children.empty()});
        const bool setOpen = searching || !g_state.collapsed.contains(set->id.value);
        if (setOpen) {
            for (const model::NodeId id : points) {
                const model::Node* node = snapshot.graph.node(id);
                if (node != nullptr) {
                    g_state.rows.push_back(TreeRow{id, node->name, 2, false});
                }
            }
        }
    }

    g_state.cachedGeneration = snapshot.graph.generation();
    g_state.cachedSearch = queryText;
    g_state.cachedMode = g_state.hierarchyMode;
    g_state.cachedHiddenRevision = g_state.hiddenRevision;
    g_state.rowsValid = true;
}

[[nodiscard]] bool chip(const char* label, bool active, bool enabled = true) noexcept {
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.28F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.42F));
    }
    const bool pressed = ImGui::SmallButton(label);
    if (active) {
        ImGui::PopStyleColor(2);
    }
    if (!enabled) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Provider unavailable in this build");
        }
    }
    return enabled && pressed;
}

void hierarchy_tabs() noexcept {
    const auto tab = [](const char* label, HierarchyMode mode) noexcept {
        const bool active = g_state.hierarchyMode == mode;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Text, kSelection);
        }
        const bool pressed = ImGui::Selectable(label, active, 0, {0.0F, scaled(22.0F)});
        if (active) {
            ImGui::PopStyleColor();
        }
        if (pressed) {
            g_state.hierarchyMode = mode;
            g_state.rowsValid = false;
        }
    };

    const float width = ImGui::GetContentRegionAvail().x / 3.0F;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(width);
    const float start = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(start);
    ImGui::BeginChild("##hierarchy_world", {width, scaled(22.0F)}, false,
                      ImGuiWindowFlags_NoScrollbar);
    tab("World", HierarchyMode::world);
    ImGui::EndChild();
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::BeginChild("##hierarchy_source", {width, scaled(22.0F)}, false,
                      ImGuiWindowFlags_NoScrollbar);
    tab("Source", HierarchyMode::source);
    ImGui::EndChild();
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::BeginChild("##hierarchy_activity", {0.0F, scaled(22.0F)}, false,
                      ImGuiWindowFlags_NoScrollbar);
    tab("Activity", HierarchyMode::activity);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

void quick_filters() noexcept {
    ImGui::BeginDisabled();
    (void)chip("Geometry", false, false);
    ImGui::SameLine();
    (void)chip("Entities", false, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (chip("Spawns", g_state.showSpawns)) {
        g_state.showSpawns = !g_state.showSpawns;
        g_state.rowsValid = false;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled();
    (void)chip("Triggers", false, false);
    ImGui::SameLine();
    (void)chip("Audio", false, false);
    ImGui::EndDisabled();

    if (chip("Hidden", g_state.showHidden)) {
        g_state.showHidden = !g_state.showHidden;
        g_state.rowsValid = false;
    }
    ImGui::SameLine();
    if (chip("Errors", g_state.errorsOnly)) {
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

model::NodeId draw_tree() noexcept {
    rebuild_rows();
    model::NodeId hovered{};
    if (g_state.rows.empty()) {
        ImGui::TextDisabled("No objects match the current view.");
        return hovered;
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_state.rows.size()), scaled(kTreeRowHeight));
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const TreeRow& row = g_state.rows[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            const float rowHeight = scaled(kTreeRowHeight);
            const float indent = scaled(kTreeIndent) * static_cast<float>(row.depth);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            for (std::uint8_t depth = 0; depth < row.depth; ++depth) {
                const float x = rowStart.x + scaled(kTreeIndent) * (static_cast<float>(depth) + 0.5F);
                drawList->AddLine({x, rowStart.y}, {x, rowStart.y + rowHeight}, kGuideColor);
            }
            ImGui::SetCursorScreenPos({rowStart.x + indent, rowStart.y});
            draw_disclosure(row, {rowStart.x + indent, rowStart.y}, rowHeight);
            ImGui::SameLine(0.0F, 1.0F);
            const bool selected = g_state.selection.selected() == row.id;
            const model::Node* node = world().graph.node(row.id);
            if (node != nullptr && node->kind == model::NodeKind::spawnPoint) {
                ImGui::PushStyleColor(ImGuiCol_Text, hidden(row.id) ? kMuted : kSpawn);
            }
            if (ImGui::Selectable(row.label.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  {0.0F, rowHeight})) {
                g_state.selection.select(row.id);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    focus(row.id);
                }
            }
            if (node != nullptr && node->kind == model::NodeKind::spawnPoint) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) {
                hovered = row.id;
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    g_state.selection.select(row.id);
                    g_state.contextTarget = row.id;
                    g_state.contextRequested = true;
                }
            }
            ImGui::PopID();
        }
    }
    return hovered;
}

void draw_outliner() noexcept {
    ImGui::TextDisabled("SCENE TREE");
    hierarchy_tabs();
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint("##world_search",
                                 "Search or type:spawn  tag:80806730",
                                 g_state.search.data(),
                                 g_state.search.size())) {
        g_state.rowsValid = false;
    }
    quick_filters();
    ImGui::Separator();
    const model::NodeId hovered = draw_tree();
    if (hovered) {
        g_state.selection.hover(hovered);
    }
}

void property_row(const char* name, const char* value) noexcept {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", name);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", value);
}

void property_u64(const char* name, std::uint64_t value, int width = 16) noexcept {
    std::array<char, 32> text{};
    (void)std::snprintf(text.data(), text.size(), "0x%0*llX", width,
                        static_cast<unsigned long long>(value));
    property_row(name, text.data());
}

void property_i32(const char* name, std::int32_t value) noexcept {
    std::array<char, 24> text{};
    (void)std::snprintf(text.data(), text.size(), "%d", value);
    property_row(name, text.data());
}

void begin_properties() noexcept {
    ImGui::BeginTable("##properties", 2,
                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg
                          | ImGuiTableFlags_BordersInnerH);
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, scaled(112.0F));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
}

void draw_identity(const model::Node& node) noexcept {
    if (!ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    begin_properties();
    property_u64("Inspection ID", node.id.value);
    property_row("Type", model::kind_name(node.kind));
    property_row("Status", model::status_name(node.status));
    if (node.runtimeEntity.has_value()) {
        property_u64("Runtime entity", *node.runtimeEntity);
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
    ImGui::EndTable();
}

void draw_transform(const model::Node& node) noexcept {
    if (!node.transform.has_value()
        || !ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    const auto& position = node.transform->position;
    std::array<char, 96> text{};
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%.4f, %.4f, %.4f",
                        static_cast<double>(position[0]),
                        static_cast<double>(position[1]),
                        static_cast<double>(position[2]));
    begin_properties();
    property_row("Position", text.data());
    ImGui::EndTable();
}

void draw_source(const model::Node& node) noexcept {
    const model::Source& source = node.source;
    if (source.packageName.empty() && source.mapStem.empty() && !source.scenarioTag.has_value()
        && !source.spawnSetHash.has_value()) {
        return;
    }
    if (!ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    begin_properties();
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
    ImGui::EndTable();
}

void draw_activity(const model::Node& node) noexcept {
    const model::Source& source = node.source;
    if (!source.activitySession.has_value() && !source.activityIndex.has_value()) {
        return;
    }
    if (!ImGui::CollapsingHeader("Activity", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    begin_properties();
    if (source.activitySession.has_value()) {
        property_u64("Session", *source.activitySession);
    }
    if (source.activityIndex.has_value()) {
        property_i32("Definition index", *source.activityIndex);
    }
    property_row("Relationship", source.spawnSetHash.has_value() ? "Destination spawn-set hash"
                                                                 : "Unknown");
    ImGui::EndTable();
}

void draw_rendering(const model::Node& node) noexcept {
    if (!model::supports(node.actions, model::Action::hide)
        || !ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    bool visible = !hidden(node.id);
    if (ImGui::Checkbox("Inspector helper visible", &visible)) {
        toggle_hidden(node.id);
    }
    ImGui::TextDisabled("Game-render visibility is unavailable.");
}

void draw_inspector_actions(const model::Node& node) noexcept {
    const camera::Status status = camera::status();
    if (model::supports(node.actions, model::Action::focus)) {
        ImGui::BeginDisabled(!status.active || !node.transform.has_value());
        if (ImGui::SmallButton("Focus")) {
            focus(node.id);
        }
        ImGui::EndDisabled();
    }
    if (model::supports(node.actions, model::Action::hide)) {
        ImGui::SameLine();
        if (ImGui::SmallButton(hidden(node.id) ? "Show" : "Hide")) {
            toggle_hidden(node.id);
        }
    }
    if (model::supports(node.actions, model::Action::isolate)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Isolate")) {
            isolate(node.id);
        }
    }
}

void draw_inspector() noexcept {
    ImGui::TextDisabled("INSPECTOR");
    const model::Node* node = selected_node();
    if (node == nullptr) {
        ImGui::Separator();
        ImGui::TextDisabled("Select an object in the Scene Tree or viewport.");
        return;
    }

    const std::string breadcrumb = world().graph.breadcrumb(node->id);
    ImGui::TextDisabled("%s", breadcrumb.c_str());
    ImGui::TextUnformatted(node->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%s", model::kind_name(node->kind));
    draw_inspector_actions(*node);
    ImGui::Separator();
    draw_identity(*node);
    draw_transform(*node);
    draw_rendering(*node);
    draw_activity(*node);
    draw_source(*node);
}

void data_row(const char* field,
              const char* type,
              const char* value,
              const char* status = "known") noexcept {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(field);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", type);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    ImGui::TextDisabled("-");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
    ImGui::TableNextColumn();
    ImGui::TextColored(std::strcmp(status, "known") == 0 ? kSelection : kWarning,
                       "%s",
                       status);
}

void draw_references() noexcept {
    ImGui::TextUnformatted("Referenced By");
    ImGui::TextDisabled("No proven incoming references are retained by this provider.");
    ImGui::Spacing();
    ImGui::TextUnformatted("References");
    ImGui::TextDisabled("No proven outgoing references are retained by this provider.");
}

void draw_data() noexcept {
    const model::Node* node = selected_node();
    if (node == nullptr) {
        ImGui::TextDisabled("Select an object to inspect structural data.");
        return;
    }
    if (!ImGui::BeginTable("##data_table",
                           6,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner
                               | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field");
    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Offset");
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Status");
    ImGui::TableHeadersRow();

    std::array<char, 96> value{};
    (void)std::snprintf(value.data(), value.size(), "0x%016llX",
                        static_cast<unsigned long long>(node->id.value));
    data_row("inspection_id", "u64", value.data());
    data_row("kind", "enum", model::kind_name(node->kind));
    if (node->nameHash.has_value()) {
        (void)std::snprintf(value.data(), value.size(), "0x%08X", *node->nameHash);
        data_row("name_hash", "fnv32", value.data());
    }
    if (node->transform.has_value()) {
        const auto& position = node->transform->position;
        (void)std::snprintf(value.data(),
                            value.size(),
                            "%.4f, %.4f, %.4f",
                            static_cast<double>(position[0]),
                            static_cast<double>(position[1]),
                            static_cast<double>(position[2]));
        data_row("position", "vec3", value.data());
    }
    if (node->source.scenarioTag.has_value()) {
        (void)std::snprintf(value.data(), value.size(), "0x%08X", *node->source.scenarioTag);
        data_row("scenario_tag", "tag32", value.data());
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Source offsets and encoded sizes are unavailable in the reduced catalog.");
}

void draw_diagnostics() noexcept {
    const auto& diagnostics = world().diagnostics;
    if (diagnostics.empty()) {
        ImGui::TextDisabled("No inspector diagnostics for this world snapshot.");
        return;
    }
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

void draw_bottom_dock() noexcept {
    const auto tab = [](const char* label, BottomTab tabValue) noexcept {
        const bool active = g_state.bottomTab == tabValue;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Text, kSelection);
        }
        if (ImGui::Selectable(label, active, 0, {scaled(104.0F), scaled(22.0F)})) {
            g_state.bottomTab = tabValue;
        }
        if (active) {
            ImGui::PopStyleColor();
        }
    };
    tab("References", BottomTab::references);
    ImGui::SameLine(0.0F, 0.0F);
    tab("Data", BottomTab::data);
    ImGui::SameLine(0.0F, 0.0F);
    tab("Diagnostics", BottomTab::diagnostics);
    ImGui::Separator();
    switch (g_state.bottomTab) {
    case BottomTab::references:
        draw_references();
        break;
    case BottomTab::data:
        draw_data();
        break;
    case BottomTab::diagnostics:
        draw_diagnostics();
        break;
    }
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
        ImGui::BeginDisabled(!status.active || !node->transform.has_value());
        if (ImGui::MenuItem("Focus", "F")) {
            focus(node->id);
        }
        ImGui::EndDisabled();
    }
    if (model::supports(node->actions, model::Action::hide)) {
        if (ImGui::MenuItem(hidden(node->id) ? "Show" : "Hide", "H")) {
            toggle_hidden(node->id);
        }
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
        && ImGui::MenuItem("Copy ID")) {
        copy_id(node->id);
    }
    if (node->tag.has_value() && model::supports(node->actions, model::Action::copyTag)
        && ImGui::MenuItem("Copy Tag Hash")) {
        copy_tag(*node->tag);
    }
    if (node->transform.has_value()
        && model::supports(node->actions, model::Action::copyPosition)
        && ImGui::MenuItem("Copy Position")) {
        copy_position(*node->transform);
    }
    ImGui::EndPopup();
}

void handle_shortcuts() noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    const model::NodeId selected = g_state.selection.selected();
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

void draw_toolbar(const camera::Status& status) noexcept {
    ImGui::BeginChild("##world_toolbar",
                      {0.0F, scaled(kToolbarHeight)},
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginMenuBar()) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        drawList->AddCircleFilled({cursor.x + scaled(5.0F), cursor.y + scaled(8.0F)},
                                  scaled(3.0F),
                                  ImGui::GetColorU32(world().stale ? kWarning : kSpawn));
        ImGui::Dummy({scaled(12.0F), 0.0F});
        ImGui::SameLine();
        const char* package = world().packageName.empty() ? "No world" : world().packageName.c_str();
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
        ImGui::BeginDisabled();
        (void)ImGui::MenuItem("Perspective");
        (void)ImGui::MenuItem("Fitted game view");
        ImGui::EndDisabled();
        if (ImGui::BeginMenu("Overlays")) {
            if (ImGui::MenuItem("Spawn helpers", nullptr, &g_state.showSpawns)) {
                g_state.rowsValid = false;
            }
            ImGui::MenuItem("Helper labels", nullptr, &g_state.showLabels);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Workspace")) {
            if (ImGui::MenuItem("Bottom dock", nullptr, !g_state.bottomCollapsed)) {
                g_state.bottomCollapsed = !g_state.bottomCollapsed;
                persist_layout();
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

void splitter(const char* id,
              float length,
              bool vertical,
              float& value,
              float direction,
              float minimum,
              float maximum) noexcept {
    const ImVec2 size = vertical ? ImVec2{scaled(kSplitterThickness), length}
                                 : ImVec2{length, scaled(kSplitterThickness)};
    ImGui::InvisibleButton(id, size);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                              ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(ImGuiCol_Separator));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        const float delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        value = std::clamp(value + delta * direction, minimum, maximum);
        g_state.layoutDirty = true;
    }
    if (g_state.layoutDirty && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        persist_layout();
    }
}

void draw_status(const camera::Status& status) noexcept {
    ImGui::BeginChild("##world_status",
                      {0.0F, scaled(kStatusHeight)},
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
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
    ImGui::SameLine();
    ImGui::TextDisabled("| %.0f FPS", static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::SameLine();
    ImGui::TextDisabled("| %zu objects", world().graph.nodes().size());
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
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width - scaled(10.0F)));
    ImGui::TextColored(kSelection, "%s", readOnly);
    ImGui::EndChild();
}

void push_editor_style() noexcept {
    ImGui::PushFont(nullptr, 13.5F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {scaled(6.0F), scaled(4.0F)});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {scaled(6.0F), scaled(3.0F)});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025F, 0.029F, 0.035F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.84F, 0.87F, 0.91F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, kMuted);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075F, 0.086F, 0.102F, 0.975F));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.075F, 0.086F, 0.102F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.165F, 0.192F, 0.227F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.165F, 0.192F, 0.227F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.102F, 0.118F, 0.141F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.145F, 0.169F, 0.200F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.22F));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.31F));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          ImVec4(kSelection.x, kSelection.y, kSelection.z, 0.38F));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.102F, 0.118F, 0.141F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145F, 0.169F, 0.200F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kSelection);
}

void pop_editor_style() noexcept {
    ImGui::PopStyleColor(15);
    ImGui::PopStyleVar(7);
    ImGui::PopFont();
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
    const bool rebuilt = g_state.provider.refresh();
    if (rebuilt) {
        g_state.selection.reconcile(world().graph);
        g_state.hidden.clear();
        g_state.collapsed.clear();
        g_state.hiddenRevision = 0;
        g_state.rowsValid = false;
    }
    g_state.selection.clear_hover();

    const camera::Status cameraStatus = camera::status();
    const renderer::frame_capture::View capturedFrame = renderer::captured_frame_locked();
    push_editor_style();
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 0.0F || displaySize.y <= 0.0F) {
        pop_editor_style();
        return false;
    }
    ImGui::SetNextWindowPos({0.0F, 0.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(displaySize, ImGuiCond_Always);
    const bool submit = ImGui::Begin("World Inspector", nullptr, kWorkspaceFlags);
    if (submit) {
        draw_toolbar(cameraStatus);

        const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
        const float statusHeight = scaled(kStatusHeight);
        const float splitterSize = scaled(kSplitterThickness);
        const float availableHeight = ImGui::GetContentRegionAvail().y;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float bottomSplit = g_state.bottomCollapsed ? 0.0F : splitterSize;
        const float maximumVisibleBottom =
            (std::max)(0.0F,
                       availableHeight - statusHeight - scaled(140.0F) - bottomSplit);
        const float bottomHeight = g_state.bottomCollapsed
                                       ? 0.0F
                                       : (std::min)(g_state.bottomHeight, maximumVisibleBottom);
        const float mainHeight =
            (std::max)(1.0F, availableHeight - statusHeight - bottomHeight - bottomSplit);
        const float minimumLeft = scaled(client::viewer::kMinimumInspectorLeftWidth);
        const float maximumLeft = scaled(client::viewer::kMaximumInspectorLeftWidth);
        const float minimumRight = scaled(client::viewer::kMinimumInspectorRightWidth);
        const float maximumRight = scaled(client::viewer::kMaximumInspectorRightWidth);
        const float minimumCenter = scaled(kMinimumViewportWidth);
        g_state.leftWidth = std::clamp(g_state.leftWidth,
                                       minimumLeft,
                                       (std::max)(minimumLeft,
                                                  availableWidth - minimumCenter - minimumRight
                                                      - splitterSize * 2.0F));
        g_state.rightWidth = std::clamp(g_state.rightWidth,
                                        minimumRight,
                                        (std::max)(minimumRight,
                                                   availableWidth - minimumCenter - g_state.leftWidth
                                                       - splitterSize * 2.0F));
        g_state.leftWidth = (std::min)(g_state.leftWidth, maximumLeft);
        g_state.rightWidth = (std::min)(g_state.rightWidth, maximumRight);
        const float centerWidth = (std::max)(1.0F,
                                             availableWidth - g_state.leftWidth - g_state.rightWidth
                                                 - splitterSize * 2.0F);

        ImGui::SetCursorScreenPos(contentOrigin);
        ImGui::BeginChild("##world_outliner",
                          {g_state.leftWidth, mainHeight},
                          ImGuiChildFlags_Borders);
        draw_outliner();
        ImGui::EndChild();

        ImGui::SetCursorScreenPos({contentOrigin.x + g_state.leftWidth, contentOrigin.y});
        splitter("##left_splitter",
                 mainHeight,
                 true,
                 g_state.leftWidth,
                 1.0F,
                 minimumLeft,
                 maximumLeft);

        const float viewportX = contentOrigin.x + g_state.leftWidth + splitterSize;
        ImGui::SetCursorScreenPos({viewportX, contentOrigin.y});
        const ImVec4 viewportBackground = capturedFrame
                                              ? ImVec4(0.025F, 0.029F, 0.035F, 1.0F)
                                              : ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, viewportBackground);
        ImGui::BeginChild("##world_viewport",
                          {centerWidth, mainHeight},
                          ImGuiChildFlags_Borders,
                          capturedFrame ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoBackground);
        const viewport::Options overlayOptions{g_state.showSpawns, g_state.showLabels};
        const viewport::Result interaction = viewport::draw(world().graph,
                                                              g_state.selection.selected(),
                                                              g_state.hidden,
                                                              cameraStatus,
                                                              capturedFrame,
                                                              overlayOptions,
                                                              g_state.viewportNavigation);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        g_state.viewportNavigation = interaction.navigation;
        viewer_input::set_workspace_navigation(g_state.viewportNavigation);
        if (interaction.hovered) {
            g_state.selection.hover(interaction.hovered);
        }
        if (interaction.selected) {
            g_state.selection.select(interaction.selected);
        }
        if (interaction.focused) {
            select_and_focus(interaction.focused);
        }
        if (interaction.context) {
            g_state.selection.select(interaction.context);
            g_state.contextTarget = interaction.context;
            g_state.contextRequested = true;
        }

        const float rightSplitterX = viewportX + centerWidth;
        ImGui::SetCursorScreenPos({rightSplitterX, contentOrigin.y});
        splitter("##right_splitter",
                 mainHeight,
                 true,
                 g_state.rightWidth,
                 -1.0F,
                 minimumRight,
                 maximumRight);
        ImGui::SetCursorScreenPos({rightSplitterX + splitterSize, contentOrigin.y});
        ImGui::BeginChild("##world_properties",
                          {g_state.rightWidth, mainHeight},
                          ImGuiChildFlags_Borders);
        draw_inspector();
        ImGui::EndChild();

        float statusY = contentOrigin.y + mainHeight;
        if (!g_state.bottomCollapsed) {
            ImGui::SetCursorScreenPos({contentOrigin.x, statusY});
            splitter("##bottom_splitter",
                     availableWidth,
                     false,
                     g_state.bottomHeight,
                     -1.0F,
                     scaled(client::viewer::kMinimumInspectorBottomHeight),
                     scaled(client::viewer::kMaximumInspectorBottomHeight));
            statusY += splitterSize;
            ImGui::SetCursorScreenPos({contentOrigin.x, statusY});
            ImGui::BeginChild("##world_bottom",
                              {availableWidth, bottomHeight},
                              ImGuiChildFlags_Borders);
            draw_bottom_dock();
            ImGui::EndChild();
            statusY += bottomHeight;
        }
        ImGui::SetCursorScreenPos({contentOrigin.x, statusY});
        draw_status(cameraStatus);
        if (g_state.contextRequested) {
            ImGui::OpenPopup("##world_inspector_context");
            g_state.contextRequested = false;
        }
        draw_node_context_menu();
        handle_shortcuts();
    }
    ImGui::End();
    pop_editor_style();
    return true;
}

} // namespace sunrise::client::ui::world_inspector
