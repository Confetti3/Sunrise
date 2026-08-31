#pragma once

#include <string_view>

namespace sunrise::client::player::console {

/** Name prefix these entries carry, which is also what releases them. */
inline constexpr std::string_view kPrefix = "player.";

/**
 * Publishes the player settings as console variables.
 *
 * They read and write through this module's own `get` and `publish`, so the console and the
 * Player page are looking at one value rather than two copies of it.
 *
 * @return True when every setting was published.
 */
[[nodiscard]] bool initialize() noexcept;

/** Removes the player entries. */
void shutdown() noexcept;

} // namespace sunrise::client::player::console
