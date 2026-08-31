#pragma once

#include <string_view>

namespace sunrise::server::character::console {

inline constexpr std::string_view kPrefix = "character.";

/** Publishes the account roster and pre-sign-in character selection. */
[[nodiscard]] bool initialize() noexcept;

void shutdown() noexcept;

} // namespace sunrise::server::character::console
