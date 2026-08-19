#pragma once

#include <cstdint>

namespace sunrise::client::ui::components::key_picker {

/**
 * Draws one virtual-key picker. Escape clears the binding; mouse buttons are not captured.
 * @return True when a new binding was captured.
 */
[[nodiscard]] bool control(const char* id, std::uint32_t& virtualKey, float width) noexcept;

} // namespace sunrise::client::ui::components::key_picker
