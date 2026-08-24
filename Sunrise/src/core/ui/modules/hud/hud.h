#pragma once

namespace sunrise::core::ui::modules::hud {

using ExtensionCallback = void (*)() noexcept;

/**
 * Publishes an optional feature-owned section at the end of the HUD page.
 * @param callback Static-lifetime frame callback, or null to remove the section.
 */
void set_extension(ExtensionCallback callback) noexcept;

/**
 * Loads the saved overlay switches, then registers the page.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 * @return True when the Core HUD page owns its registry slot.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Removes the Core HUD page and drops the switch file path. */
void shutdown() noexcept;

} // namespace sunrise::core::ui::modules::hud
