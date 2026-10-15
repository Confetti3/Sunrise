#include "activity_host_sdk_scene_inputs.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>
#include <string_view>

#include "../../../state/activity_sdk/scene_triggers.h"

namespace sunrise::server::ui::activity_host::sdk_mission_view {
namespace {

namespace mission = server::activity::activity_sdk_mission;
namespace sdk = state::activity_sdk;

/** Writes a trigger's name, or KEY_ and its hash when the name is unknown. */
void trigger_label(std::uint32_t key, std::array<char, 72>& output) noexcept {
    const std::string_view name = sdk::scene_trigger_name(key);
    if (name.empty()) {
        std::snprintf(output.data(), output.size(), "KEY_%08X", static_cast<unsigned>(key));
    } else {
        std::snprintf(output.data(),
                      output.size(),
                      "%.*s",
                      static_cast<int>((std::min)(name.size(), output.size() - 1U)),
                      name.data());
    }
}

} // namespace

/** Draws one scene's Inputs button and its popup. */
mission::SceneStatus draw_scene_inputs(const sdk::BoundView& view,
                                       std::uint32_t occurrenceRow,
                                       std::uint32_t slotRow,
                                       bool available,
                                       bool& sent) noexcept {
    sent = false;
    mission::SceneStatus status = mission::SceneStatus::ready;
    if (view.catalog == nullptr) {
        return status;
    }
    const auto gates = sdk::scene_gates(view.catalog->authored_scene_event_keys(), slotRow);
    const auto inputs = static_cast<std::size_t>(
        std::count_if(gates.begin(), gates.end(), &sdk::scene_trigger_is_input));
    std::array<char, 32> button{};
    std::snprintf(button.data(), button.size(), "Inputs (%zu)", inputs);
    ImGui::BeginDisabled(inputs == 0);
    if (ImGui::SmallButton(button.data())) {
        ImGui::OpenPopup("##scene_inputs");
    }
    ImGui::EndDisabled();
    if (!ImGui::BeginPopup("##scene_inputs")) {
        return status;
    }
    std::array<char, 72> label{};
    ImGui::TextDisabled("Fire an input in the running scene. Start it first.");
    ImGui::BeginDisabled(!available);
    for (const sdk::format::AuthoredSceneEventKey& gate : gates) {
        if (!sdk::scene_trigger_is_input(gate)) {
            continue;
        }
        trigger_label(gate.key, label);
        ImGui::PushID(static_cast<int>(gate.key));
        if (ImGui::Button(label.data())) {
            status = mission::activate_authored_scene(view, occurrenceRow, slotRow, gate.key);
            sent = true;
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("Outputs it reports");
    bool anyOutput = false;
    for (const sdk::format::AuthoredSceneEventKey& gate : gates) {
        if (sdk::scene_trigger_is_output(gate)) {
            trigger_label(gate.key, label);
            ImGui::BulletText("%s", label.data());
            anyOutput = true;
        }
    }
    if (!anyOutput) {
        ImGui::TextDisabled("none");
    }
    ImGui::EndPopup();
    return status;
}

} // namespace sunrise::server::ui::activity_host::sdk_mission_view
