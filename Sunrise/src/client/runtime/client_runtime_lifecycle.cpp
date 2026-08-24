#include "../../core/logging/log.h"
#include "../content/investment/worker.h"
#include "../hooks/assert_handler/assert_handler_lifecycle.h"
#include "../hooks/bitmap/bitmap_hook_lifecycle.h"
#include "../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../hooks/config_getter/config_getter_lifecycle.h"
#include "../hooks/cursor/runtime.h"
#include "../hooks/graphics/graphics_hook_lifecycle.h"
#include "../hooks/graphics/renderer/native_debug_renderer.h"
#include "../hooks/inactivity/inactivity_override.h"
#include "../hooks/infinite_ammo/infinite_ammo.h"
#include "../hooks/network/runtime.h"
#include "../hooks/noclip/runtime.h"
#include "../hooks/package_trust/package_trust_bypass.h"
#include "../hooks/player_hold/player_hold.h"
#include "../hooks/polled_input/runtime.h"
#include "../hooks/presentation/presentation.h"
#include "../hooks/queuez/queuez_hook_lifecycle.h"
#include "../hooks/retail_log/retail_log_lifecycle.h"
#include "../hooks/teleport/runtime.h"
#include "../hooks/viewer_audio/viewer_audio.h"
#include "../hooks/viewer_camera/viewer_camera.h"
#include "../hooks/viewer_objects/viewer_objects.h"
#include "../hooks/viewer_triggers/viewer_triggers.h"
#include "../inactivity/inactivity_settings_store.h"
#include "../inspection/inspection_capture.h"
#include "../inspection/inspection_settings_store.h"
#include "../inspection/providers/activity_graph_inspection.h"
#include "../inspection/providers/activity_logic_inspection.h"
#include "../inspection/providers/bubble_bounds_inspection.h"
#include "../movement/movement_settings_store.h"
#include "../player/player_settings_store.h"
#include "../targets/game.h"
#include "../targets/steam_targets.h"
#include "../ui/runtime/client_ui_module_runtime.h"
#include "../viewer/viewer_camera_path_store.h"
#include "../viewer/viewer_camera_settings_store.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client {

/** Initializes Client-owned process state without installing hooks. */
bool initialize(void* module) noexcept {
    // Loaded before the pages register, so each page draws saved values on its first frame.
    movement::initialize(module);
    player::initialize(module);
    inactivity::initialize(module);
    viewer::initialize(module);
    viewer::paths::initialize(module);
    inspection::settings::initialize(module);
    inspection::providers::activity_graph::initialize(module);
    inspection::providers::bubble_bounds::initialize(module);
    inspection::providers::activity_logic::initialize(module);
    inspection::capture::initialize(module);
    return ui::runtime::initialize();
}

/** Detaches Client hooks before clearing their resolved target entries. */
bool shutdown() noexcept {
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=client phase=begin");
    AcquireSRWLockExclusive(&runtime::g_lock);
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=graphics_hooks phase=begin");
    if (!hooks::graphics::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=graphics_hooks result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=graphics_hooks result=ok");
    if (!hooks::graphics::renderer::native_debug::uninstall_observer()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=native_render_observer result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=viewer_audio phase=begin");
    if (!viewer::audio::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=viewer_audio result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=viewer_audio result=ok");
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=viewer_camera phase=begin");
    if (!viewer::camera::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=viewer_camera result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=viewer_camera result=ok");
    hooks::player_hold::reset();
    // Viewer Camera no longer owns input, so the shared input hooks can now detach.
    hooks::cursor::uninstall();
    hooks::polled_input::uninstall();
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=presentation phase=begin");
    if (!hooks::presentation::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=presentation result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=presentation result=ok");
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=network_hooks phase=begin");
    if (!hooks::network::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=network_hooks result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=network_hooks result=ok");
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=package_trust phase=begin");
    if (!hooks::package_trust::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=package_trust result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=package_trust result=ok");
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=remaining_hooks phase=begin");
    hooks::bitmap::uninstall();
    hooks::bootflow::uninstall();
    hooks::infinite_ammo::uninstall();
    hooks::inactivity::uninstall();
    if (!hooks::noclip::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=noclip result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    hooks::teleport::uninstall();
    if (!viewer::triggers::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=viewer_triggers result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    viewer::objects::uninstall();
    hooks::queuez::uninstall();
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=config_getter phase=begin");
    if (!hooks::config_getter::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=config_getter result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=assert_handler phase=begin");
    if (!hooks::assert_handler::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=assert_handler result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=retail_log phase=begin");
    if (!hooks::retail_log::uninstall()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=shutdown stage=retail_log result=fail");
        ReleaseSRWLockExclusive(&runtime::g_lock);
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=shutdown stage=remaining_hooks result=ok");
    content::investment::worker::reset();
    targets::steam::clear();
    if (runtime::g_platformModule != nullptr) {
        if (FreeLibrary(runtime::g_platformModule) == FALSE) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::error,
                             "ev=shutdown stage=steam_module result=fail");
            ReleaseSRWLockExclusive(&runtime::g_lock);
            return false;
        }
        runtime::g_platformModule = nullptr;
    }
    targets::game::retail_log::clear();
    targets::game::content::clear();
    targets::game::network::clear();
    runtime::g_mainStage = runtime::StageState::pending;
    runtime::g_graphicsStage = runtime::StageState::pending;
    runtime::g_platformStage = runtime::StageState::pending;
    ui::runtime::shutdown();
    // The reverse of the order the stores initialize in.
    inspection::capture::shutdown();
    inspection::providers::activity_logic::shutdown();
    inspection::providers::bubble_bounds::shutdown();
    inspection::providers::activity_graph::shutdown();
    inspection::settings::shutdown();
    viewer::paths::shutdown();
    viewer::shutdown();
    inactivity::shutdown();
    player::shutdown();
    movement::shutdown();
    core::log::write(core::log::Channel::client, core::log::Level::info, "ev=shutdown result=ok");
    ReleaseSRWLockExclusive(&runtime::g_lock);
    return true;
}

} // namespace sunrise::client
