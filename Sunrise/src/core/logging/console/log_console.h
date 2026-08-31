#pragma once

#include <string_view>

namespace sunrise::core::log::console {

/** Name prefix these entries carry, which is also what releases them. */
inline constexpr std::string_view kPrefix = "log.";

/**
 * Publishes one console variable per logging channel.
 *
 * Thresholds are the logging setting worth reaching without a restart: the events that explain a
 * problem usually sit below the level configured before it appeared.
 *
 * @return True when every channel was published.
 */
[[nodiscard]] bool initialize() noexcept;

/** Removes the logging entries. */
void shutdown() noexcept;

} // namespace sunrise::core::log::console
