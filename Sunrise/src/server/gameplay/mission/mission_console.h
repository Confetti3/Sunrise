#pragma once

#include <string_view>

namespace sunrise::server::gameplay::mission::console {

/** Name prefix these entries carry, which is also what releases them. */
inline constexpr std::string_view kPrefix = "mission.";

/** Publishes mission inspection and live-reload commands. */
[[nodiscard]] bool initialize() noexcept;

/** Removes the mission entries. */
void shutdown() noexcept;

} // namespace sunrise::server::gameplay::mission::console
