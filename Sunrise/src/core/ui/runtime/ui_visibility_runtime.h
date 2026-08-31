#pragma once

#include <Windows.h>

#include "settings.h"

namespace sunrise::core::ui::runtime {

/** A copy of the UI visibility state, taken under the lock, for the drawing code. */
struct VisibilitySnapshot {
    bool initialized{};
    bool enabled{};
    bool visible{};
    /** Insert is the binding before initialize runs. */
    UINT toggleVirtualKey{VK_INSERT};
    /**
     * Whether the console is showing. It opens and closes on its own binding, so a reader can
     * type a line without the menu covering what the line changes.
     */
    bool consoleVisible{};
    /** The F10 key is the binding before initialize runs. */
    UINT consoleToggleVirtualKey{VK_F10};
};

/**
 * Reports whether any surface has the reader's attention.
 *
 * Cursor handling and polled input take one answer, not a list of surfaces. Deciding here what
 * counts as open is what lets a surface be added without either of them learning about it.
 *
 * @param state Visibility state to read.
 * @return True while the menu or the console is showing.
 */
[[nodiscard]] constexpr bool interface_open(const VisibilitySnapshot& state) noexcept {
    return state.visible || state.consoleVisible;
}

/**
 * Reports whether an active UI owns a key as one of its surface toggles.
 * @param state Visibility state read once by the input callback.
 * @param virtualKey Windows key code to test.
 * @return True only after initialization, while UI input is enabled, for an active binding.
 */
[[nodiscard]] constexpr bool owns_toggle_key(const VisibilitySnapshot& state,
                                             UINT virtualKey) noexcept {
    return state.initialized && state.enabled
           && (virtualKey == state.toggleVirtualKey
               || virtualKey == state.consoleToggleVirtualKey);
}

/**
 * Starts visibility from Core settings, after checking them.
 * @param settings Enabled state and Windows virtual-key binding.
 * @return True when the binding is a usable Windows virtual key.
 */
[[nodiscard]] bool initialize(const Settings& settings) noexcept;

/** Clears visibility and key binding state so the module can unload. */
void shutdown() noexcept;

/** @return A copy of the whole visibility state, taken under the lock. */
[[nodiscard]] VisibilitySnapshot snapshot() noexcept;

/**
 * Applies one key press to the visibility state.
 *
 * A press matching both bindings is treated as the menu's, since that is the binding a reader
 * configured first and the one that existed before the console did.
 *
 * @param virtualKey Code from Client input handling.
 * @return True only when an active binding matched and visibility changed.
 */
[[nodiscard]] bool toggle_for_key(UINT virtualKey) noexcept;

} // namespace sunrise::core::ui::runtime
