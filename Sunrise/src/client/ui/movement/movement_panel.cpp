#include "movement_panel.h"

#include <imgui.h>

#include "../../../core/ui/components/label/ui_label_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../movement/movement_settings_store.h"
#include "../components/key_picker/client_key_picker.h"

namespace sunrise::client::ui::movement {
namespace {

namespace label = core::ui::components::label;
namespace key_picker = client::ui::components::key_picker;

void begin_control_row(const char* text, float labelWidth) noexcept {
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(labelWidth);
}

} // namespace

void draw() noexcept {
    client::movement::Settings settings = client::movement::get();
    bool changed = false;

    ImGui::TextUnformatted("Teleport");
    ImGui::Separator();
    ImGui::TextWrapped("Teleports you forward in the facing direction. "
                       "Cancels vertical momentum.");
    ImGui::Spacing();

    changed =
        core::ui::components::toggle::control("Enabled##teleport", settings.enabled) || changed;

    // One label column and one control column, so the slider and key buttons share both edges.
    const float labelWidth =
        label::inset() + ImGui::CalcTextSize("Toggle key").x + ImGui::GetStyle().ItemSpacing.x * 2;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;

    begin_control_row("Distance", labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    changed = ImGui::SliderFloat("##distance",
                                 &settings.distance,
                                 client::movement::kMinimumDistance,
                                 client::movement::kMaximumDistance,
                                 "%.0f units")
              || changed;

    begin_control_row("Key", labelWidth);
    changed = key_picker::control("teleport_key", settings.virtualKey, controlWidth) || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Noclip");
    ImGui::Separator();
    ImGui::TextWrapped("Disable collision on the horizontal axis.");
    ImGui::Spacing();

    changed =
        core::ui::components::toggle::control("Enabled##noclip", settings.noclipEnabled) || changed;

    begin_control_row("Toggle key", labelWidth);
    changed = key_picker::control("noclip_key", settings.noclipToggleKey, controlWidth) || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Fly");
    ImGui::Separator();
    ImGui::TextWrapped("Fly with your movement keys.");
    ImGui::Spacing();

    changed = core::ui::components::toggle::control("Enabled##fly", settings.flyEnabled) || changed;

    begin_control_row("Toggle key", labelWidth);
    changed = key_picker::control("fly_key", settings.flyToggleKey, controlWidth) || changed;

    begin_control_row("Speed", labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    changed = ImGui::SliderFloat("##fly_speed",
                                 &settings.flySpeed,
                                 client::movement::kMinimumFlySpeed,
                                 client::movement::kMaximumFlySpeed,
                                 "%.0f units/s")
              || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Sword Skate Fix");
    ImGui::Separator();
    ImGui::TextWrapped("Disable sword swings blocking ability usage.");
    ImGui::Spacing();

    changed =
        core::ui::components::toggle::control("Enabled##sword_skate", settings.swordSkateEnabled)
        || changed;

    if (changed && !client::movement::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::movement
