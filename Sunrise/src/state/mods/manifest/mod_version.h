#pragma once

#include <array>
#include <string_view>

#include "../definition.h"

namespace sunrise::state::mods::version {

/** `65535.65535.65535` and its null. */
inline constexpr std::size_t kTextCapacity = 18;

/**
 * Parses `MAJOR.MINOR.PATCH`. Each part is 0..65535 with no leading zero.
 * @param text Candidate version text.
 * @param output Receives the version; unchanged on failure.
 * @return True for a complete, canonical version.
 */
[[nodiscard]] bool parse(std::string_view text, Version& output) noexcept;

/** @return Negative, zero or positive as `first` is lower, equal or higher. */
[[nodiscard]] int compare(const Version& first, const Version& second) noexcept;

/** @return True when `value` lies in the dependency's inclusive range. */
[[nodiscard]] bool satisfies(const Dependency& dependency, const Version& value) noexcept;

/** @return The null-terminated `MAJOR.MINOR.PATCH` text. */
[[nodiscard]] std::array<char, kTextCapacity> text(const Version& value) noexcept;

} // namespace sunrise::state::mods::version
