#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/hud/hud.h"
#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../hud/hud_panel.h"
#include "../movement/movement_panel.h"
#include "../player/player_panel.h"
#include "../viewer/viewer_panel.h"
#include "../world_inspector/world_inspector.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable IDs prevent Client modules from colliding with Server modules. */
constexpr std::string_view kMovementStableId = "client.movement";
constexpr std::string_view kViewerStableId = "client.viewer";
constexpr std::string_view kPlayerStableId = "client.player";
/** Short menu label for the shared teleport and noclip page. */
constexpr std::string_view kMovementDisplayName = "Movement";
/** Short menu label for the detached camera and inspection page. */
constexpr std::string_view kViewerDisplayName = "Viewer";
/** Short menu label for the player page. */
constexpr std::string_view kPlayerDisplayName = "Player";

core::ui::modules::registry::PageRegistration g_movementPage;
core::ui::modules::registry::PageRegistration g_viewerPage;
core::ui::modules::registry::PageRegistration g_playerPage;

} // namespace

/** @return True when all Client modules own their Core UI registry slots. */
bool initialize() noexcept {
    world_inspector::initialize();
    // Registered after movement, which is the order the menu lists them in.
    const bool movementOwned = g_movementPage.acquire(
        core::ui::modules::Owner::client, kMovementStableId, kMovementDisplayName, &movement::draw);
    const bool viewerOwned = g_viewerPage.acquire(
        core::ui::modules::Owner::client, kViewerStableId, kViewerDisplayName, &viewer::draw);
    const bool playerOwned = g_playerPage.acquire(
        core::ui::modules::Owner::client, kPlayerStableId, kPlayerDisplayName, &player::draw);
    if (movementOwned && viewerOwned && playerOwned) {
        core::ui::modules::hud::set_extension(&hud::draw);
    }
    return movementOwned && viewerOwned && playerOwned;
}

/** Removes the Client modules from the Core UI registry. */
void shutdown() noexcept {
    world_inspector::shutdown();
    core::ui::modules::hud::set_extension(nullptr);
    g_playerPage.release();
    g_viewerPage.release();
    g_movementPage.release();
}

} // namespace sunrise::client::ui::runtime
