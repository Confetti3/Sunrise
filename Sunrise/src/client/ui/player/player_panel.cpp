#include "player_panel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../hooks/inactivity/inactivity_override.h"
#include "../../inactivity/inactivity_settings_store.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {
namespace {

namespace inactivity = client::inactivity;
namespace toggle = core::ui::components::toggle;

/** Seven columns lay the fourteen inactivity lanes out in two rows. */
constexpr int kLaneColumns = 7;

/** @return True when the configured lane timeout changes. */
[[nodiscard]] bool draw_lane(std::size_t index,
                             inactivity::Settings& configured,
                             const hooks::inactivity::Status& status) noexcept {
    const bool orbit = index == inactivity::kOrbitLane;
    bool changed = false;
    ImGui::PushID(static_cast<int>(index));
    ImGui::BeginDisabled(orbit);
    ImGui::TextUnformatted(inactivity::kActivities[index].column.data());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", inactivity::kActivities[index].name.data());
    }
    // Highlight the live timeout unless the lane is held at its inactive maximum.
    const std::uint32_t live = status.liveValid ? status.live[index] : 0;
    const bool liveActive = status.liveValid && live != inactivity::kMaximumTimeoutMs;

    ImGui::SetNextItemWidth(-FLT_MIN);
    std::uint32_t milliseconds = configured.timeouts[index];
    if (liveActive) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    ImGui::InputScalar("##lane",
                       ImGuiDataType_U32,
                       &milliseconds,
                       nullptr,
                       nullptr,
                       "%u",
                       ImGuiInputTextFlags_CharsDecimal);
    if (liveActive) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        configured.timeouts[index] =
            std::clamp(milliseconds, inactivity::kMinimumTimeoutMs, inactivity::kMaximumTimeoutMs);
        changed = true;
    }
    ImGui::EndDisabled();
    // Keep live status readable even when editing is disabled.
    if (!status.liveValid) {
        ImGui::TextDisabled("-");
    } else if (liveActive) {
        ImGui::Text("%u", live);
    } else {
        ImGui::TextDisabled("%u", live);
    }
    ImGui::PopID();
    return changed;
}

void draw_inactivity_clocks(const hooks::inactivity::Status& status) noexcept {
    // Poll only while this section is visible.
    const hooks::inactivity::Timers timers = hooks::inactivity::timers();
    if (timers.idleValid) {
        ImGui::Text("Idle %.1f s", static_cast<double>(timers.idleMs) / 1000.0);
    } else {
        ImGui::TextDisabled("Idle -");
    }
    ImGui::SameLine();
    if (timers.sessionValid) {
        ImGui::Text("Session %.1f s", static_cast<double>(timers.sessionMs) / 1000.0);
    } else {
        ImGui::TextDisabled("Session -");
    }
    if (!status.liveGraceValid) {
        return;
    }
    const bool passed =
        status.liveGraceMs == 0 || (timers.sessionValid && timers.sessionMs > status.liveGraceMs);
    ImGui::SameLine();
    // Reported, never written.
    const double grace = static_cast<double>(status.liveGraceMs) / 1000.0;
    if (passed) {
        ImGui::TextDisabled("Grace %.1f s (passed)", grace);
    } else {
        ImGui::Text("Grace %.1f s (no kick until then)", grace);
    }
}

void draw_inactivity() noexcept {
    inactivity::Settings configured = inactivity::get();
    // Use one status snapshot for the whole section.
    const hooks::inactivity::Status status = hooks::inactivity::status();

    ImGui::TextUnformatted("Inactivity");
    ImGui::Separator();
    ImGui::TextWrapped("Disable AFK timeouts from activities kicking to orbit and the title "
                       "screen.");
    ImGui::Spacing();

    // The two switches are exclusive, so turning this one on drops the set timeouts.
    bool changed = toggle::control("Enabled##inactivity", configured.enabled);
    if (changed && configured.enabled) {
        configured.custom = false;
    }

    if (ImGui::CollapsingHeader("Advanced##inactivity")) {
        // A per-lane timeout has nothing to act on once every lane is already removed.
        ImGui::BeginDisabled(configured.enabled);
        if (toggle::control("Use set timeouts##inactivity_custom", configured.custom)) {
            if (configured.custom) {
                configured.enabled = false;
            }
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("In milliseconds");
        ImGui::Spacing();
        ImGui::BeginDisabled(configured.enabled || !configured.custom);
        if (ImGui::BeginTable("lanes", kLaneColumns, ImGuiTableFlags_SizingStretchSame)) {
            for (std::size_t index = 0; index < inactivity::kActivityCount; ++index) {
                ImGui::TableNextColumn();
                changed = draw_lane(index, configured, status) || changed;
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        draw_inactivity_clocks(status);
    }

    if (changed) {
        (void)inactivity::publish(configured);
    }
}

} // namespace

void draw() noexcept {
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep every weapon's reserves full.");
    ImGui::Spacing();

    const bool changed = core::ui::components::toggle::control("Enabled##infinite_ammo",
                                                               settings.infiniteAmmoEnabled);
    if (changed) {
        (void)client::player::publish(settings);
    }

    ImGui::Spacing();
    ImGui::Spacing();
    draw_inactivity();
}

} // namespace sunrise::client::ui::player
