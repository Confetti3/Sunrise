#include "bootflow_hook_lifecycle.h"

#include <atomic>

#include "../../../core/settings/settings.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

std::atomic_bool g_installed{false};

} // namespace

/**
 * Attaches the boot-step fixes that carry sign-in through to orbit.
 * Each fix stands alone at one site, so a miss on one is reported and the others still attach.
 * @return True when every fix attached.
 */
bool install() noexcept {
    // The hook runs once per boot, so the choice is made here rather than exposed as a command:
    // detaching it later would be too late to matter. holdAttached feeds the OR below, so a
    // deliberate skip still reads as "not installed" there. holdOk feeds the AND: the skip must
    // not sink it, because turning a fix off on purpose is not a failure.
    const bool holdWanted = core::settings::get().client.holdCharacterSelect;
    const bool holdAttached = holdWanted && install_character_select_hold();
    const bool holdOk = !holdWanted || holdAttached;
    const bool sliceSet = install_orbit_slice_set();
    const bool skip = install_profile_setup_skip();
    const bool composition = install_composition_check();
    const bool handoff = install_orbit_handoff();
    const bool joinReady = install_join_request_ready();
    const bool ownerSlot = install_owner_activity_slot();
    const bool regionPrivate = install_region_private();
    const bool worldStep = install_world_step();
    const bool spawn = install_spawn_hold();
    const bool fade = install_fade_release();
    const bool anyFix = holdAttached || sliceSet || skip || composition || handoff || joinReady
                        || ownerSlot || regionPrivate || worldStep || spawn || fade;
    g_installed.store(anyFix, std::memory_order_release);
    return holdOk && sliceSet && skip && composition && handoff && joinReady && ownerSlot
           && regionPrivate && worldStep && spawn && fade;
}

/** Detaches every boot-step fix, in the reverse order of install. */
void uninstall() noexcept {
    uninstall_fade_release();
    uninstall_spawn_hold();
    uninstall_world_step();
    uninstall_region_private();
    uninstall_owner_activity_slot();
    uninstall_join_request_ready();
    uninstall_orbit_handoff();
    uninstall_composition_check();
    uninstall_profile_setup_skip();
    uninstall_orbit_slice_set();
    uninstall_character_select_hold();
    g_installed.store(false, std::memory_order_release);
}

/** @return True while at least one boot-step fix is attached. */
bool is_installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::hooks::bootflow
