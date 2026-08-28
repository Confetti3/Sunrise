#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/hud/hud.h"
#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../hud/hud_panel.h"
#include "../movement/movement_panel.h"
#include "../player/player_panel.h"
#include "../world_inspector/world_inspector.h"

namespace sunrise::client::ui::runtime {
namespace {

constexpr std::string_view kMovementStableId = "client.movement";
constexpr std::string_view kInspectorStableId = "client.inspector";
constexpr std::string_view kPlayerStableId = "client.player";
constexpr std::string_view kMovementDisplayName = "Movement";
constexpr std::string_view kInspectorDisplayName = "Inspector";
constexpr std::string_view kPlayerDisplayName = "Player";

core::ui::modules::registry::PageRegistration g_movementPage;
core::ui::modules::registry::PageRegistration g_inspectorPage;
core::ui::modules::registry::PageRegistration g_playerPage;

} // namespace

bool initialize() noexcept {
    world_inspector::initialize();
    const bool movementOwned = g_movementPage.acquire(
        core::ui::modules::Owner::client, kMovementStableId, kMovementDisplayName, &movement::draw);
    const bool inspectorOwned = g_inspectorPage.acquire(core::ui::modules::Owner::client,
                                                        kInspectorStableId,
                                                        kInspectorDisplayName,
                                                        &world_inspector::draw_launcher);
    const bool playerOwned = g_playerPage.acquire(
        core::ui::modules::Owner::client, kPlayerStableId, kPlayerDisplayName, &player::draw);
    const bool requiredPagesOwned = movementOwned && inspectorOwned && playerOwned;
    if (requiredPagesOwned) {
        core::ui::modules::hud::set_extension(&hud::draw);
    }
    return requiredPagesOwned;
}

void shutdown() noexcept {
    core::ui::modules::hud::set_extension(nullptr);
    g_playerPage.release();
    g_inspectorPage.release();
    g_movementPage.release();
    world_inspector::shutdown();
}

} // namespace sunrise::client::ui::runtime
