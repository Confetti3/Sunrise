#pragma once

namespace sunrise::core::ui::modules::mods {

/** @return True when the Core Mods page owns its registry slot. */
[[nodiscard]] bool initialize() noexcept;

/** Removes the Core Mods page. */
void shutdown() noexcept;

} // namespace sunrise::core::ui::modules::mods
