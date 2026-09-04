#include "activity_host_sdk_squad_view.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../client/ui/activity/authored_placement_marker.h"
#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../activity/activity_sdk_mission_runtime.h"
#include "../../activity/activity_sdk_squad_runtime.h"
#include "../../bap/runtime.h"
#include "activity_host_anchor_render_controls.h"
#include "activity_host_table_layout.h"

namespace sunrise::server::ui::activity_host::sdk_squad_view {
namespace {

namespace format = state::activity_sdk::format;
namespace marker = client::ui::activity::authored_placement_marker;
namespace mission = server::activity::activity_sdk_mission;
namespace render_controls = server::ui::activity_host::anchor_render_controls;
namespace runtime = server::activity::activity_sdk_squads;
namespace scaling = core::ui::scaling::dpi;
namespace section = core::ui::components::section;
namespace sdk = state::activity_sdk;
namespace squad_auth = middleware::bap::activity_message::squad_auth;
namespace tables = middleware::content::packages::tables;

constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
constexpr ImGuiTableFlags kWideTableFlags =
    kTableFlags | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;

std::weak_ptr<const sdk::Catalog> g_selectionCatalog{};
mission::SceneStatus g_behaviorSceneResult{mission::SceneStatus::ready};
bool g_hasBehaviorSceneResult{};
state::activity::SessionBinding g_selectionBinding{};
std::uint64_t g_selectionClientGeneration{};
std::uint32_t g_selectedSquad{format::kAbsentIndex};
std::uint32_t g_initializedSquad{format::kAbsentIndex};
std::array<int, squad_auth::kMaximumRequestedCountLength> g_requestedCounts{};
int g_mode{};
bool g_useNameHash{};
std::uint32_t g_nameHash{};
runtime::Status g_lastResult{runtime::Status::ready};
bool g_hasResult{};
ImGuiTextFilter g_squadFilter{};
bool g_currentStateOnly{};

/** One filtered global squad row and whether its occurrence belongs to the live region. */
struct BrowserRow final {
    std::uint32_t squadRow{format::kAbsentIndex};
    bool currentState{};
};

std::vector<BrowserRow> g_listedSquads{};
/** Every anchor this scenario can draw. The search must never change it. */
std::vector<marker::Anchor> g_scenarioAnchors{};
/** Anchors owned by the rows the search currently lists, for the bulk-select action only. */
std::vector<marker::Anchor> g_listedAnchors{};
/** The scenario anchor set is fixed for one binding, so it is rebuilt only when that changes. */
bool g_scenarioAnchorsValid{};

/** @return The global row index of one borrowed squad row. */
[[nodiscard]] std::uint32_t global_row(const sdk::Catalog& catalog,
                                       const format::Squad& squad) noexcept {
    const auto squads = catalog.squads();
    const std::size_t row = static_cast<std::size_t>(&squad - squads.data());
    return row < squads.size() ? static_cast<std::uint32_t>(row) : format::kAbsentIndex;
}

/** @return True when one global row belongs to the bound scenario subset. */
[[nodiscard]] bool contains(std::span<const format::Squad> squads,
                            const sdk::Catalog& catalog,
                            std::uint32_t row) noexcept {
    return std::any_of(squads.begin(), squads.end(), [&catalog, row](const auto& squad) noexcept {
        return global_row(catalog, squad) == row;
    });
}

/** Resets selection and action inputs whenever the exact SDK binding changes. */
void sync_selection(const sdk::BoundView& view, std::span<const format::Squad> squads) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const std::uint32_t first =
        squads.empty() ? format::kAbsentIndex : global_row(catalog, squads.front());
    if (g_selectionCatalog.lock() != view.catalog || !same_binding(g_selectionBinding, view.binding)
        || g_selectionClientGeneration != view.activityClientGeneration) {
        g_selectionCatalog = view.catalog;
        g_selectionBinding = view.binding;
        g_selectionClientGeneration = view.activityClientGeneration;
        g_selectedSquad = first;
        g_initializedSquad = format::kAbsentIndex;
        g_hasResult = false;
        g_scenarioAnchorsValid = false;
    }
    if (!contains(squads, catalog, g_selectedSquad)) {
        g_selectedSquad = first;
        g_initializedSquad = format::kAbsentIndex;
        g_hasResult = false;
    }
}

/** @return One source slot retained by a generated squad row. */
[[nodiscard]] const format::Slot* source_slot(const sdk::Catalog& catalog,
                                              const format::Squad& squad) noexcept {
    const auto slots = catalog.slots();
    return squad.slotIndex < slots.size() ? &slots[squad.slotIndex] : nullptr;
}

/** @return The source slot's strongest generated display label. */
[[nodiscard]] std::string_view source_label(const sdk::Catalog& catalog,
                                            const format::Squad& squad) noexcept {
    const format::Slot* const slot = source_slot(catalog, squad);
    if (slot != nullptr) {
        const std::string_view name = catalog.string(slot->name);
        if (!name.empty()) {
            return name;
        }
        const std::string_view id = catalog.string(slot->id);
        if (!id.empty()) {
            return id;
        }
    }
    return catalog.string(squad.id);
}

/** Appends one nonempty generated string to a row-wide search document. */
void append_search_text(std::string& text, std::string_view value) {
    if (value.empty()) {
        return;
    }
    text.push_back(' ');
    text.append(value.data(), value.size());
}

/** Appends one bounded formatted identity fragment to a row-wide search document. */
template <typename... Arguments>
void append_search_format(std::string& text, const char* pattern, Arguments... arguments) {
    std::array<char, 512> value{};
    const int length = std::snprintf(value.data(), value.size(), pattern, arguments...);
    if (length <= 0) {
        return;
    }
    text.push_back(' ');
    text.append(value.data(), (std::min)(static_cast<std::size_t>(length), value.size() - 1));
}

/** Builds the complete generated identity document consumed by ImGuiTextFilter. */
[[nodiscard]] bool squad_matches_filter(const sdk::Catalog& catalog,
                                        const format::Squad& squad,
                                        std::uint32_t squadRow) {
    if (!g_squadFilter.IsActive()) {
        return true;
    }
    std::string search;
    search.reserve(2'048);
    append_search_text(search, catalog.string(squad.id));
    append_search_format(search,
                         "squad %u row %u spawner 0x%08X spawn-rule 0x%08X flags 0x%08X",
                         static_cast<unsigned>(squadRow),
                         static_cast<unsigned>(squadRow),
                         static_cast<unsigned>(squad.spawnerConfigTag),
                         static_cast<unsigned>(squad.spawnRuleConfigTag),
                         static_cast<unsigned>(squad.flags));

    const auto slots = catalog.slots();
    if (squad.slotIndex < slots.size()) {
        const format::Slot& slot = slots[squad.slotIndex];
        append_search_text(search, catalog.string(slot.name));
        append_search_text(search, catalog.string(slot.id));
        for (const format::Text& alias : sdk::slot_aliases(catalog, slot)) {
            append_search_text(search, catalog.string(alias.value));
        }
        append_search_format(search,
                             "slot-row %u slot %u type %u component 0x%08X sense 0x%08X auth "
                             "0x%08X",
                             static_cast<unsigned>(squad.slotIndex),
                             static_cast<unsigned>(slot.slotIndex),
                             static_cast<unsigned>(slot.slotType),
                             static_cast<unsigned>(slot.componentClass),
                             static_cast<unsigned>(slot.senseSchema),
                             static_cast<unsigned>(slot.authSchema));
    }

    const auto objects = catalog.objects();
    if (squad.objectIndex < objects.size()) {
        const format::Object& object = objects[squad.objectIndex];
        append_search_text(search, catalog.string(object.id));
        append_search_format(search,
                             "object-row %u object-tag 0x%08X object-key 0x%08X",
                             static_cast<unsigned>(squad.objectIndex),
                             static_cast<unsigned>(object.objectTag),
                             static_cast<unsigned>(object.objectKey));
    }

    const auto occurrences = catalog.occurrences();
    if (squad.occurrenceIndex < occurrences.size()) {
        const format::Occurrence& occurrence = occurrences[squad.occurrenceIndex];
        append_search_text(search, catalog.string(occurrence.id));
        append_search_text(search, catalog.string(occurrence.contextRegistryKey));
        append_search_text(search, catalog.string(occurrence.registryId));
        append_search_text(search, catalog.string(occurrence.entryId));
        append_search_format(search,
                             "occurrence-row %u registry-field 0x%08X object-ordinal %u bubble-row "
                             "%u state-row %u",
                             static_cast<unsigned>(squad.occurrenceIndex),
                             static_cast<unsigned>(occurrence.registryField),
                             static_cast<unsigned>(occurrence.objectOrdinal),
                             static_cast<unsigned>(occurrence.bubbleIndex),
                             static_cast<unsigned>(occurrence.stateIndex));
    }

    const auto actorClasses = catalog.actor_classes();
    for (const format::SquadMember& member : sdk::squad_members(catalog, squad)) {
        append_search_text(search, catalog.string(member.id));
        append_search_format(search,
                             "member %u key 0x%08X actor-row %u",
                             static_cast<unsigned>(member.memberOrdinal),
                             static_cast<unsigned>(member.memberKey),
                             static_cast<unsigned>(member.actorClassIndex));
        if (member.actorClassIndex < actorClasses.size()) {
            const format::ActorClass& actor = actorClasses[member.actorClassIndex];
            append_search_text(search, catalog.string(actor.id));
            append_search_format(search,
                                 "actor 0x%08X name-hash 0x%08X rsat 0x%08X",
                                 static_cast<unsigned>(actor.definitionTag),
                                 static_cast<unsigned>(actor.nameHash),
                                 static_cast<unsigned>(actor.rsatTag));
        }
    }

    for (const format::SquadAnchor& anchor : sdk::squad_anchors(catalog, squad)) {
        append_search_text(search, catalog.string(anchor.id));
        const float x = std::bit_cast<float>(anchor.positionBits[0]);
        const float y = std::bit_cast<float>(anchor.positionBits[1]);
        const float z = std::bit_cast<float>(anchor.positionBits[2]);
        append_search_format(search,
                             "point %u 0x%08X[%u] identity 0x%016llX position %.3f %.3f %.3f "
                             "%.9g %.9g %.9g",
                             static_cast<unsigned>(anchor.pointOrdinal),
                             static_cast<unsigned>(anchor.objectListTag),
                             static_cast<unsigned>(anchor.placementOrdinal),
                             static_cast<unsigned long long>(anchor.placedEntryIdentity),
                             static_cast<double>(x),
                             static_cast<double>(y),
                             static_cast<double>(z),
                             static_cast<double>(x),
                             static_cast<double>(y),
                             static_cast<double>(z));
    }
    return g_squadFilter.PassFilter(search.data(), search.data() + search.size());
}

/** Resolves the generated bubble/state ordinals owned by one squad occurrence. */
[[nodiscard]] bool squad_region(const sdk::Catalog& catalog,
                                const format::Squad& squad,
                                std::uint32_t& bubbleOrdinal,
                                std::uint32_t& stateOrdinal) noexcept {
    bubbleOrdinal = 0;
    stateOrdinal = 0;
    const auto occurrences = catalog.occurrences();
    const auto bubbles = catalog.bubbles();
    const auto states = catalog.states();
    if (squad.occurrenceIndex >= occurrences.size()) {
        return false;
    }
    const format::Occurrence& occurrence = occurrences[squad.occurrenceIndex];
    if (occurrence.scenarioIndex != squad.scenarioIndex
        || occurrence.objectIndex != squad.objectIndex || occurrence.bubbleIndex >= bubbles.size()
        || occurrence.stateIndex >= states.size()) {
        return false;
    }
    const format::Bubble& bubble = bubbles[occurrence.bubbleIndex];
    const format::State& state = states[occurrence.stateIndex];
    if (bubble.scenarioIndex != squad.scenarioIndex || state.scenarioIndex != squad.scenarioIndex
        || state.bubbleIndex != occurrence.bubbleIndex) {
        return false;
    }
    bubbleOrdinal = bubble.bubbleOrdinal;
    stateOrdinal = state.stateOrdinal;
    return true;
}

/** @return True when one squad occurrence belongs to the live ActivityClient region. */
[[nodiscard]] bool squad_is_current(const sdk::Catalog& catalog,
                                    const format::Squad& squad,
                                    std::int32_t effectiveRegion) noexcept {
    if (effectiveRegion < 0) {
        return false;
    }
    std::uint32_t bubbleOrdinal = 0;
    std::uint32_t stateOrdinal = 0;
    const std::uint32_t region = static_cast<std::uint32_t>(effectiveRegion);
    return squad_region(catalog, squad, bubbleOrdinal, stateOrdinal)
           && bubbleOrdinal == region / tables::kSliceSetIndexFactor
           && stateOrdinal == region % tables::kSliceSetIndexFactor;
}

/** Appends every exact anchor child of one squad row. */
void append_squad_anchors(const sdk::BoundView& view,
                          std::uint32_t squadRow,
                          const format::Squad& squad,
                          std::vector<marker::Anchor>& output) {
    for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
        marker::Anchor anchor{};
        if (marker::sdk_squad_anchor(view, squadRow, squad.anchors.first + ordinal, anchor)) {
            output.push_back(anchor);
        }
    }
}

/** Builds the listed rows, and separately the full scenario row set the world may draw. */
[[nodiscard]] bool materialize_rows(const sdk::BoundView& view,
                                    std::span<const format::Squad> squads,
                                    std::int32_t effectiveRegion,
                                    std::size_t& currentCount) noexcept {
    currentCount = 0;
    try {
        const sdk::Catalog& catalog = *view.catalog;
        for (const format::Squad& squad : squads) {
            currentCount += squad_is_current(catalog, squad, effectiveRegion) ? 1U : 0U;
        }
        std::vector<BrowserRow> listed;
        listed.reserve(squads.size());
        std::vector<marker::Anchor> scenarioAnchors;
        std::vector<marker::Anchor> listedAnchors;
        for (const bool currentPass : {true, false}) {
            for (const format::Squad& squad : squads) {
                const bool current = squad_is_current(catalog, squad, effectiveRegion);
                const std::uint32_t row = global_row(catalog, squad);
                if (current != currentPass || row == format::kAbsentIndex) {
                    continue;
                }
                if (!g_scenarioAnchorsValid) {
                    append_squad_anchors(view, row, squad, scenarioAnchors);
                }
                if ((current || !g_currentStateOnly) && squad_matches_filter(catalog, squad, row)) {
                    listed.push_back({row, current});
                    append_squad_anchors(view, row, squad, listedAnchors);
                }
            }
        }
        g_listedSquads.swap(listed);
        if (!g_scenarioAnchorsValid) {
            g_scenarioAnchors.swap(scenarioAnchors);
            g_scenarioAnchorsValid = true;
        }
        g_listedAnchors.swap(listedAnchors);
        return true;
    } catch (...) {
        return false;
    }
}

/** Counts exact anchor children from one squad that are in the copied marker set. */
[[nodiscard]] std::size_t selected_anchor_count(const sdk::BoundView& view,
                                                std::uint32_t squadRow,
                                                const marker::Context& context,
                                                const marker::State& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (squadRow >= catalog.squads().size()) {
        return 0;
    }
    const format::Squad& squad = catalog.squads()[squadRow];
    std::size_t count = 0;
    for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
        marker::Anchor anchor{};
        const std::uint32_t anchorRow = squad.anchors.first + ordinal;
        if (marker::sdk_squad_anchor(view, squadRow, anchorRow, anchor)
            && marker::contains(selected,
                                context,
                                marker::AnchorSource::sdkSquadAnchor,
                                anchor.sourceRow,
                                anchor.ownerRow)) {
            ++count;
        }
    }
    return count;
}

/** Ticks or clears every exact anchor child of one squad. It never changes what Show draws. */
void set_squad_rendering(const sdk::BoundView& view,
                         std::uint32_t squadRow,
                         const marker::Context& context,
                         bool enabled,
                         marker::State& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (squadRow >= catalog.squads().size()) {
        return;
    }
    const format::Squad& squad = catalog.squads()[squadRow];
    for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
        marker::Anchor anchor{};
        const std::uint32_t anchorRow = squad.anchors.first + ordinal;
        if (!marker::sdk_squad_anchor(view, squadRow, anchorRow, anchor)) {
            continue;
        }
        const marker::State current = marker::snapshot();
        const bool present = marker::contains(current,
                                              context,
                                              marker::AnchorSource::sdkSquadAnchor,
                                              anchor.sourceRow,
                                              anchor.ownerRow);
        if (present != enabled) {
            marker::toggle({context, anchor});
        }
    }
    selected = marker::snapshot();
}

/** Publishes every scenario point and draws the shared render controls. */
void draw_world_render_controls(const marker::Context& context, marker::State& selected) noexcept {
    ImGui::PushID("sdk_squad_world_render");
    render_controls::draw_options(selected);
    const bool published =
        marker::publish_rows(context, g_scenarioAnchors, marker::PublishedSource::explicitRows);
    if (ImGui::Button("Tick listed")) {
        (void)marker::select_many(
            context, g_listedAnchors, g_listedAnchors.size() > marker::kSelectionCapacity);
        selected = marker::snapshot();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ticks every point owned by a listed squad.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Untick all")) {
        marker::clear();
        selected = marker::snapshot();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu point%s in this scenario",
                        g_scenarioAnchors.size(),
                        g_scenarioAnchors.size() == 1 ? "" : "s");
    if (!published) {
        ImGui::TextDisabled("Point source unavailable");
    }
    render_controls::draw_status(context, selected);
    ImGui::PopID();
}

/** Draws the list filters. They choose what the table shows and nothing else. */
void draw_squad_filters() noexcept {
    ImGui::SetNextItemWidth(scaling::pixels(360.0F));
    g_squadFilter.Draw("Search##sdk_squads");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Filters the list. The world still draws the same points.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Current state only##sdk_squads", &g_currentStateOnly);
}

/** Draws the listed squads, their world-point ticks, and the row highlight. */
void draw_squad_table(const sdk::BoundView& view,
                      std::span<const format::Squad> squads,
                      const marker::Context& context,
                      std::int32_t effectiveRegion,
                      std::size_t currentCount,
                      marker::State& selected) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    if (effectiveRegion >= 0) {
        ImGui::TextDisabled("%zu listed of %zu; %zu in the current region %d",
                            g_listedSquads.size(),
                            squads.size(),
                            currentCount,
                            effectiveRegion);
    } else {
        ImGui::TextDisabled(
            "%zu listed of %zu; current region unavailable", g_listedSquads.size(), squads.size());
    }
    if (squads.empty()) {
        ImGui::TextDisabled("This scenario has no squads.");
        return;
    }
    if (!ImGui::BeginTable(
            "##sdk_squads", 8, kWideTableFlags, table_layout::size(g_listedSquads.size()))) {
        return;
    }
    ImGui::TableSetupColumn("Draw");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("region");
    ImGui::TableSetupColumn("slot");
    ImGui::TableSetupColumn("spawner");
    ImGui::TableSetupColumn("spawn rule");
    ImGui::TableSetupColumn("members");
    ImGui::TableSetupColumn("points");
    table_layout::frozen_headers();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_listedSquads.size()));
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const BrowserRow browserRow = g_listedSquads[static_cast<std::size_t>(visible)];
            const std::uint32_t row = browserRow.squadRow;
            if (row >= catalog.squads().size()) {
                continue;
            }
            const format::Squad& squad = catalog.squads()[row];
            const format::Slot* const slot = source_slot(catalog, squad);
            const std::string_view label = source_label(catalog, squad);
            const std::string_view visibleLabel = label.empty() ? "unnamed squad" : label;
            std::array<char, 192> selectable{};
            (void)std::snprintf(selectable.data(),
                                selectable.size(),
                                "%.*s##squad_%u",
                                static_cast<int>(visibleLabel.size()),
                                visibleLabel.data(),
                                static_cast<unsigned>(row));

            std::size_t exactAnchors = 0;
            if (context.catalogKind == marker::CatalogKind::activitySdk) {
                for (std::uint32_t ordinal = 0; ordinal < squad.anchors.count; ++ordinal) {
                    marker::Anchor anchor{};
                    exactAnchors +=
                        marker::sdk_squad_anchor(view, row, squad.anchors.first + ordinal, anchor)
                            ? 1U
                            : 0U;
                }
            }
            const std::size_t selectedCount = selected_anchor_count(view, row, context, selected);
            bool rendered = selectedCount != 0;

            ImGui::PushID(static_cast<int>(row));
            table_layout::next_row();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(exactAnchors == 0);
            if (ImGui::Checkbox("##render", &rendered)) {
                set_squad_rendering(view, row, context, rendered, selected);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (exactAnchors == 0) {
                    ImGui::SetTooltip("This squad has no points.");
                } else {
                    ImGui::SetTooltip("%zu of %zu points ticked", selectedCount, exactAnchors);
                }
            }
            ImGui::TableNextColumn();
            if (table_layout::selectable(selectable.data(), g_selectedSquad == row)) {
                g_selectedSquad = row;
                g_initializedSquad = format::kAbsentIndex;
                g_hasResult = false;
            }
            ImGui::TableNextColumn();
            std::uint32_t bubbleOrdinal = 0;
            std::uint32_t stateOrdinal = 0;
            if (!squad_region(catalog, squad, bubbleOrdinal, stateOrdinal)) {
                ImGui::TextDisabled("invalid");
            } else if (browserRow.currentState) {
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),
                                   "current %u/%u",
                                   static_cast<unsigned>(bubbleOrdinal),
                                   static_cast<unsigned>(stateOrdinal));
            } else {
                ImGui::Text("%u/%u",
                            static_cast<unsigned>(bubbleOrdinal),
                            static_cast<unsigned>(stateOrdinal));
            }
            ImGui::TableNextColumn();
            if (slot == nullptr) {
                ImGui::TextDisabled("invalid row %u", static_cast<unsigned>(squad.slotIndex));
            } else {
                ImGui::Text("%u / type %u",
                            static_cast<unsigned>(slot->slotIndex),
                            static_cast<unsigned>(slot->slotType));
            }
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(squad.spawnerConfigTag));
            ImGui::TableNextColumn();
            ImGui::Text("0x%08X", static_cast<unsigned>(squad.spawnRuleConfigTag));
            ImGui::TableNextColumn();
            ImGui::Text("%zu", sdk::squad_members(catalog, squad).size());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", sdk::squad_anchors(catalog, squad).size());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/** Initializes member requests from the exact positive generated defaults. */
void initialize_inputs(const sdk::Catalog& catalog,
                       const format::Squad& squad,
                       std::uint32_t squadRow) noexcept {
    if (g_initializedSquad == squadRow) {
        return;
    }
    g_requestedCounts.fill(0);
    const auto members = sdk::squad_members(catalog, squad);
    for (std::size_t index = 0; index < members.size() && index < g_requestedCounts.size();
         ++index) {
        g_requestedCounts[index] = (std::max)(members[index].defaultCount, 0);
    }
    g_mode = 0;
    g_useNameHash = false;
    g_nameHash = 0;
    g_initializedSquad = squadRow;
    g_hasResult = false;
}

/** @return The conservative authored count accepted by every candidate lane. */
[[nodiscard]] std::uint16_t authored_maximum(const format::SquadMember& member) noexcept {
    return *std::min_element(member.candidateCounts.begin(), member.candidateCounts.end());
}

/** Draws bounded member-count inputs and copies their current wire vector. */
[[nodiscard]] std::size_t draw_members(const sdk::Catalog& catalog,
                                       const format::Squad& squad,
                                       std::span<std::int32_t> output,
                                       bool& hasPositive) noexcept {
    const auto members = sdk::squad_members(catalog, squad);
    const auto actorClasses = catalog.actor_classes();
    const std::size_t count = (std::min)(members.size(), output.size());
    hasPositive = false;
    for (std::size_t index = 0; index < count; ++index) {
        const int maximum = static_cast<int>(authored_maximum(members[index]));
        g_requestedCounts[index] = std::clamp(g_requestedCounts[index], 0, maximum);
        output[index] = static_cast<std::int32_t>(g_requestedCounts[index]);
        hasPositive = hasPositive || output[index] > 0;
    }
    if (!ImGui::BeginTable("##sdk_squad_members", 4, kWideTableFlags, table_layout::size(count))) {
        return count;
    }
    ImGui::TableSetupColumn("member", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("actor class", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("authored default");
    ImGui::TableSetupColumn("requested", ImGuiTableColumnFlags_WidthStretch);
    table_layout::frozen_headers();
    for (std::size_t index = 0; index < count; ++index) {
        const format::SquadMember& member = members[index];
        const int maximum = static_cast<int>(authored_maximum(member));
        table_layout::next_row();
        ImGui::TableNextColumn();
        const std::string_view memberId = catalog.string(member.id);
        if (memberId.empty()) {
            ImGui::Text("member %u", static_cast<unsigned>(member.memberOrdinal));
        } else {
            ImGui::TextUnformatted(memberId.data(), memberId.data() + memberId.size());
        }
        ImGui::TableNextColumn();
        if (member.actorClassIndex >= actorClasses.size()) {
            ImGui::TextDisabled(member.actorClassIndex == format::kAbsentIndex
                                    ? "unresolved"
                                    : "invalid actor row");
        } else {
            const std::string_view actorId =
                catalog.string(actorClasses[member.actorClassIndex].id);
            if (actorId.empty()) {
                ImGui::TextDisabled("actor row %u", static_cast<unsigned>(member.actorClassIndex));
            } else {
                ImGui::TextUnformatted(actorId.data(), actorId.data() + actorId.size());
            }
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", member.defaultCount);
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::SliderInt("##requested", &g_requestedCounts[index], 0, maximum)) {
            output[index] = static_cast<std::int32_t>(g_requestedCounts[index]);
            g_hasResult = false;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    hasPositive = std::any_of(
        output.begin(), output.begin() + count, [](auto value) noexcept { return value > 0; });
    return count;
}

/** Draws exact authored anchors without assigning gameplay semantics to their positions. */
void draw_anchors(const sdk::Catalog& catalog, const format::Squad& squad) noexcept {
    const auto anchors = sdk::squad_anchors(catalog, squad);
    ImGui::Text("%zu authored anchor%s", anchors.size(), anchors.size() == 1 ? "" : "s");
    for (const format::SquadAnchor& anchor : anchors) {
        const float x = std::bit_cast<float>(anchor.positionBits[0]);
        const float y = std::bit_cast<float>(anchor.positionBits[1]);
        const float z = std::bit_cast<float>(anchor.positionBits[2]);
        ImGui::BulletText("point %u  0x%08X[%u]  (%.3f, %.3f, %.3f)",
                          static_cast<unsigned>(anchor.pointOrdinal),
                          static_cast<unsigned>(anchor.objectListTag),
                          static_cast<unsigned>(anchor.placementOrdinal),
                          static_cast<double>(x),
                          static_cast<double>(y),
                          static_cast<double>(z));
    }
}

/** Draws every bounded option carried by the native type-1 encoder. */
void draw_place_action(const sdk::BoundView& view,
                       const format::Squad& squad,
                       std::uint32_t squadRow) noexcept {
    const auto members = sdk::squad_members(*view.catalog, squad);
    if (ImGui::Button("Authored defaults")) {
        for (std::size_t index = 0; index < members.size() && index < g_requestedCounts.size();
             ++index) {
            g_requestedCounts[index] = (std::max)(members[index].defaultCount, 0);
        }
        g_hasResult = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zero all")) {
        g_requestedCounts.fill(0);
        g_hasResult = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Maximum all")) {
        for (std::size_t index = 0; index < members.size() && index < g_requestedCounts.size();
             ++index) {
            g_requestedCounts[index] = static_cast<int>(authored_maximum(members[index]));
        }
        g_hasResult = false;
    }
    std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength> requested{};
    bool hasPositive = false;
    const std::size_t count = draw_members(*view.catalog, squad, requested, hasPositive);
    const std::span<const std::int32_t> counts(requested.data(), count);
    constexpr std::array<const char*, 2> kModes{"Mode 0", "Mode 2"};
    g_mode = std::clamp(g_mode, 0, static_cast<int>(kModes.size()) - 1);
    ImGui::SetNextItemWidth(scaling::pixels(180.0F));
    if (ImGui::Combo("Placement mode", &g_mode, kModes.data(), static_cast<int>(kModes.size()))) {
        g_hasResult = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The native schema accepts authored modes 0 and 2.");
    }
    if (ImGui::Checkbox("Include definition name hash", &g_useNameHash)) {
        g_hasResult = false;
    }
    if (g_useNameHash) {
        ImGui::SetNextItemWidth(scaling::pixels(180.0F));
        if (ImGui::InputScalar("Name hash",
                               ImGuiDataType_U32,
                               &g_nameHash,
                               nullptr,
                               nullptr,
                               "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal)) {
            g_hasResult = false;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Optional type-1 definition hash. It is not the slot name.");
        }
    }
    const squad_auth::Mode mode = g_mode == 0 ? squad_auth::Mode::mode0 : squad_auth::Mode::mode2;
    const std::optional<std::uint32_t> nameHash =
        g_useNameHash ? std::optional<std::uint32_t>{g_nameHash} : std::nullopt;
    const runtime::Status available = runtime::availability(view, squadRow, counts, mode, nameHash);
    const bool enabled = available == runtime::Status::ready && hasPositive;
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Button("Place squad")) {
        g_lastResult = runtime::place(view, squadRow, counts, mode, nameHash);
        g_hasResult = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!hasPositive && available == runtime::Status::ready) {
        ImGui::TextDisabled("set at least one count above zero");
    } else {
        ImGui::TextDisabled("%s", runtime::status_name(available));
    }
    if (g_hasResult) {
        ImGui::Text("Last place  %s", runtime::status_name(g_lastResult));
    }
}

/** Offers every exact type-43 scene which authors behavior for the selected type-1 squad. */
void draw_authored_behavior_scenes(const sdk::BoundView& view,
                                   const format::Squad& squad) noexcept {
    mission::Snapshot snapshot{};
    if (mission::query(view, snapshot) != mission::Status::ready || view.catalog == nullptr) {
        return;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto slots = catalog.slots();
    const auto occurrences = catalog.occurrences();
    std::uint32_t squadSlotRow = format::kAbsentIndex;
    for (std::uint32_t row = 0; row < slots.size(); ++row) {
        const format::Slot& slot = slots[row];
        if (slot.objectIndex == squad.objectIndex && slot.slotType == format::kSquadSlotType
            && slot.slotIndex == squad.slotIndex) {
            squadSlotRow = row;
            break;
        }
    }
    if (squadSlotRow == format::kAbsentIndex) {
        return;
    }
    section::header("Package-linked scenes", nullptr);
    ImGui::TextWrapped("Companion scenes, for an A/B test. Their effect on AI is unverified.");
    std::size_t sceneCount = 0;
    for (std::uint32_t sceneSlotRow = 0; sceneSlotRow < slots.size(); ++sceneSlotRow) {
        const format::Slot& sceneSlot = slots[sceneSlotRow];
        if (sceneSlot.slotType != format::kAuthoredSceneSlotType) {
            continue;
        }
        bool linked = false;
        for (const format::AuthoredSceneSquadEdge& edge :
             sdk::slot_authored_scene_squad_edges(catalog, sceneSlot)) {
            linked = linked || edge.squadSlotIndex == squadSlotRow;
        }
        if (!linked) {
            continue;
        }
        for (std::uint32_t occurrenceRow = 0; occurrenceRow < occurrences.size(); ++occurrenceRow) {
            const format::Occurrence& occurrence = occurrences[occurrenceRow];
            if (occurrence.scenarioIndex != view.scenarioRow
                || occurrence.stateIndex != snapshot.plan.stateRow
                || occurrence.objectIndex != sceneSlot.objectIndex) {
                continue;
            }
            ++sceneCount;
            ImGui::PushID(static_cast<int>(sceneSlotRow));
            ImGui::PushID(static_cast<int>(occurrenceRow));
            const mission::SceneStatus available =
                mission::authored_scene_availability(view, occurrenceRow, sceneSlotRow);
            ImGui::BeginDisabled(available != mission::SceneStatus::ready);
            if (ImGui::Button("Run authored behavior")) {
                g_behaviorSceneResult =
                    mission::activate_authored_scene(view, occurrenceRow, sceneSlotRow);
                g_hasBehaviorSceneResult = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const std::string_view name = catalog.string(sceneSlot.name);
            ImGui::Text("%.*s  %s",
                        static_cast<int>(name.size()),
                        name.data(),
                        mission::status_name(available));
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    if (sceneCount != 0) {
        ImGui::TextDisabled(
            "Type-1 placement has no movement goal field. Run a linked scene to see "
            "what the package pairs with it.");
        if (g_hasBehaviorSceneResult) {
            ImGui::Text("Last behavior scene  %s", mission::status_name(g_behaviorSceneResult));
        }
    }
}

} // namespace

/** Draws generated scenario squads and the guarded server-side place action. */
void draw(const sdk::BoundView& view, const format::Scenario& scenario) noexcept {
    const sdk::Catalog& catalog = *view.catalog;
    const auto squads = sdk::scenario_squads(catalog, scenario);
    sync_selection(view, squads);
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(view.binding, link);
    draw_squad_filters();
    std::size_t currentCount = 0;
    if (!materialize_rows(view, squads, link.effectiveRegion, currentCount)) {
        ImGui::TextDisabled("Squad row storage did not fit");
        marker::publish_no_rows();
        return;
    }
    marker::Context context{};
    marker::State selected = marker::snapshot();
    if (marker::sdk_context(view, context)) {
        draw_world_render_controls(context, selected);
    } else {
        ImGui::TextDisabled("The world cannot draw this activity");
        marker::publish_no_rows();
    }
    draw_squad_table(view, squads, context, link.effectiveRegion, currentCount, selected);
    if (g_selectedSquad >= catalog.squads().size()) {
        return;
    }
    const format::Squad& squad = catalog.squads()[g_selectedSquad];
    initialize_inputs(catalog, squad, g_selectedSquad);
    const std::string_view label = source_label(catalog, squad);
    const std::string_view visibleLabel = label.empty() ? "unnamed squad" : label;
    section::header("Place this squad", nullptr);
    ImGui::Text("%.*s", static_cast<int>(visibleLabel.size()), visibleLabel.data());
    draw_anchors(catalog, squad);
    draw_place_action(view, squad, g_selectedSquad);
    draw_authored_behavior_scenes(view, squad);
    if (ImGui::TreeNodeEx("Technical details##squad", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::Text("squad row %u  spawner 0x%08X  spawn rule 0x%08X  flags 0x%08X",
                    static_cast<unsigned>(g_selectedSquad),
                    static_cast<unsigned>(squad.spawnerConfigTag),
                    static_cast<unsigned>(squad.spawnRuleConfigTag),
                    static_cast<unsigned>(squad.flags));
        ImGui::TextUnformatted("wire mode 0");
        ImGui::TreePop();
    }
}

} // namespace sunrise::server::ui::activity_host::sdk_squad_view
