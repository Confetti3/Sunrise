#include "core/ui/runtime/ui_visibility_runtime.h"

namespace runtime = sunrise::core::ui::runtime;

int main() {
    runtime::shutdown();
    const runtime::VisibilitySnapshot stopped = runtime::snapshot();
    if (runtime::owns_toggle_key(stopped, VK_INSERT)
        || runtime::owns_toggle_key(stopped, VK_F10)
        || stopped.consoleToggleVirtualKey != VK_F10) {
        return 10;
    }
    runtime::Settings settings{};
    settings.toggleVirtualKey = VK_INSERT;
    settings.consoleToggleVirtualKey = VK_OEM_3;
    if (!runtime::initialize(settings)) {
        return 1;
    }

    if (runtime::interface_open(runtime::snapshot()) || runtime::toggle_for_key(VK_F1)) {
        return 2;
    }
    if (!runtime::toggle_for_key(VK_OEM_3)) {
        return 3;
    }
    const runtime::VisibilitySnapshot consoleOpen = runtime::snapshot();
    if (consoleOpen.visible || !consoleOpen.consoleVisible || !runtime::interface_open(consoleOpen)) {
        return 4;
    }
    if (!runtime::toggle_for_key(VK_OEM_3)) {
        return 5;
    }
    if (runtime::interface_open(runtime::snapshot())) {
        return 6;
    }

    settings.toggleVirtualKey = VK_F1;
    settings.consoleToggleVirtualKey = VK_F1;
    if (!runtime::initialize(settings) || !runtime::toggle_for_key(VK_F1)) {
        return 7;
    }
    const runtime::VisibilitySnapshot sharedBinding = runtime::snapshot();
    if (!sharedBinding.visible || sharedBinding.consoleVisible) {
        return 8;
    }

    settings.enabled = false;
    if (!runtime::initialize(settings) || runtime::toggle_for_key(VK_F1)
        || runtime::interface_open(runtime::snapshot())
        || runtime::owns_toggle_key(runtime::snapshot(), VK_F1)) {
        return 9;
    }
    settings.enabled = true;
    settings.consoleToggleVirtualKey = 0;
    if (runtime::initialize(settings)) {
        return 11;
    }
    settings.consoleToggleVirtualKey = 0xFF;
    if (runtime::initialize(settings)) {
        return 12;
    }
    runtime::shutdown();
    return 0;
}
