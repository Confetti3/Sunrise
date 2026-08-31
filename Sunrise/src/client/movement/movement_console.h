#pragma once

#include <string_view>

namespace sunrise::client::movement::console {

/** Name prefix these entries carry, which is also what releases them. */
inline constexpr std::string_view kPrefix = "movement.";

/**
 * Publishes the movement settings as console variables.
 *
 * Each one reads and writes through this module's own `get` and `publish`, so the console and the
 * Movement page are looking at one value rather than two copies of it.
 *
 * @return True when every setting was published.
 */
[[nodiscard]] bool initialize() noexcept;

/** Removes the movement entries. */
void shutdown() noexcept;

} // namespace sunrise::client::movement::console
