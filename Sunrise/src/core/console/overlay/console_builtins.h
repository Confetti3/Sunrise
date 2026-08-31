#pragma once

#include <string_view>

namespace sunrise::core::console::builtins {

/** Name prefix the console's own entries carry, which is also what releases them. */
inline constexpr std::string_view kBuiltinPrefix = "console.";

/**
 * Publishes the console's own commands.
 *
 * The console answers for itself before any module is wired: a reader who opens it can already
 * find what exists, read what one entry means, and clear what they have read.
 *
 * @return True when every command was published.
 */
[[nodiscard]] bool initialize() noexcept;

/** Removes the console's own commands. */
void shutdown() noexcept;

} // namespace sunrise::core::console::builtins
