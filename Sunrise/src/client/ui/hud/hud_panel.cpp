/** Client-owned native game-presentation controls appended to the Core HUD page. */

#include "hud_panel.h"

#include <imgui.h>

#include "../../../core/ui/components/section/ui_section_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../hooks/presentation/presentation.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::hud {

/** Draws and persists native game-presentation switches inside the Core HUD page. */
void draw() noexcept {
    core::ui::components::section::header(
        "Game presentation", "Control the native game HUD and first-person weapon independently.");
    ImGui::Spacing();

    client::player::Settings settings = client::player::get();
    bool changed = false;
    changed = core::ui::components::toggle::control("Remove HUD##game_hud", settings.removeHud)
              || changed;
    changed = core::ui::components::toggle::control("Hide Weapon##first_person_weapon",
                                                    settings.hideWeapon)
              || changed;
    if (changed) {
        (void)client::player::publish(settings);
    }

    const hooks::presentation::Status status = hooks::presentation::status();
    if (!status.weaponReady) {
        ImGui::TextDisabled("Weapon hiding is unavailable for this build.");
    }
    if (settings.removeHud && !status.hudReady) {
        ImGui::TextDisabled("Game HUD removal is unavailable for this build.");
    } else if (settings.removeHud && status.hudFault) {
        ImGui::TextDisabled("Game HUD removal is waiting for a valid player setting.");
    }
}

} // namespace sunrise::client::ui::hud
