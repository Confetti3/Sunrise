#pragma once

#include <Windows.h>

namespace sunrise::core::ui::fonts::installed {

/** Borrowed view over the fixed Sunrise-owned installed-font bytes. */
struct DataView {
    void* bytes{};
    int byteCount{};
};

/**
 * Reads the optional face from the process directory, then one module directory.
 * @param module Loaded fallback module, or null to use only the process image.
 * @param output Receives bytes that stay valid until clear is called.
 * @return True when the whole checked OpenType face fits fixed storage.
 */
[[nodiscard]] bool load(HMODULE module, DataView& output) noexcept;

/** Wipes the installed-font bytes and all path scratch storage. */
void clear() noexcept;

} // namespace sunrise::core::ui::fonts::installed
