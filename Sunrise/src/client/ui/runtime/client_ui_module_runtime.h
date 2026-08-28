#pragma once

namespace sunrise::client::ui::runtime {

/** @return True when all Client pages own their Core UI registry slots. */
[[nodiscard]] bool initialize() noexcept;

/** Removes the Client pages and HUD extension from the Core UI registry. */
void shutdown() noexcept;

} // namespace sunrise::client::ui::runtime
