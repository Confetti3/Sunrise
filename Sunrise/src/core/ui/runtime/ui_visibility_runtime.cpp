#include "ui_visibility_runtime.h"

#include <Windows.h>

namespace sunrise::core::ui::runtime {
namespace {

/** Usable Windows virtual-key codes start at 1. */
constexpr UINT kFirstVirtualKey = 0x01;
/** Windows reserves 0xFF, so 0xFE is the last usable virtual-key code. */
constexpr UINT kLastVirtualKey = 0xFE;
/** The menu starts closed, so startup never takes game input focus. */
constexpr bool kInitialVisibility = false;

VisibilitySnapshot g_state{};
SRWLOCK g_visibilityLock{SRWLOCK_INIT};

/** @return True when the code is a usable Windows virtual key. */
[[nodiscard]] bool is_valid_virtual_key(UINT virtualKey) noexcept {
    return virtualKey >= kFirstVirtualKey && virtualKey <= kLastVirtualKey;
}

} // namespace

/** Starts visibility from Core settings, after checking them. */
bool initialize(const Settings& settings) noexcept {
    if (!is_valid_virtual_key(settings.toggleVirtualKey)
        || !is_valid_virtual_key(settings.consoleToggleVirtualKey)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_visibilityLock);
    g_state.initialized = true;
    g_state.enabled = settings.enabled;
    g_state.visible = kInitialVisibility;
    g_state.toggleVirtualKey = settings.toggleVirtualKey;
    g_state.consoleVisible = kInitialVisibility;
    g_state.consoleToggleVirtualKey = settings.consoleToggleVirtualKey;
    ReleaseSRWLockExclusive(&g_visibilityLock);
    return true;
}

/** Clears visibility and key binding state so the module can unload. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_visibilityLock);
    g_state = {};
    ReleaseSRWLockExclusive(&g_visibilityLock);
}

/** @return A copy of the whole visibility state, taken under the lock. */
VisibilitySnapshot snapshot() noexcept {
    AcquireSRWLockShared(&g_visibilityLock);
    const VisibilitySnapshot result = g_state;
    ReleaseSRWLockShared(&g_visibilityLock);
    return result;
}

/** Applies one key press to the visibility state. */
bool toggle_for_key(UINT virtualKey) noexcept {
    AcquireSRWLockExclusive(&g_visibilityLock);
    const bool active = g_state.initialized && g_state.enabled;
    const bool menuBinding = active && virtualKey == g_state.toggleVirtualKey;
    // The menu is tested first, so a key bound to both surfaces stays the menu's. That binding
    // predates the console and is the one a reader configured deliberately.
    const bool consoleBinding =
        active && !menuBinding && virtualKey == g_state.consoleToggleVirtualKey;
    if (menuBinding) {
        g_state.visible = !g_state.visible;
    } else if (consoleBinding) {
        g_state.consoleVisible = !g_state.consoleVisible;
    }
    ReleaseSRWLockExclusive(&g_visibilityLock);
    return menuBinding || consoleBinding;
}

} // namespace sunrise::core::ui::runtime
