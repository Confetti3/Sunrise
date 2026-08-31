#pragma once

#include <Windows.h>

namespace sunrise::core::ui::runtime {

/** UI visibility settings, read at boot. */
struct Settings {
    /** When off, the UI ignores the toggle key. */
    bool enabled{true};
    /** Windows virtual key that shows or hides the UI. */
    UINT toggleVirtualKey{VK_INSERT};
    /**
     * Windows virtual key that shows or hides the console.
     *
     * F10 avoids the shipped gameplay bindings. The key remains configurable for readers who
     * prefer another explicit binding.
     */
    UINT consoleToggleVirtualKey{VK_F10};
};

} // namespace sunrise::core::ui::runtime
