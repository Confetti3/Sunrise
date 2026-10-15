#pragma once

#include <string_view>

#include "mod_manifest_parser.h"

namespace sunrise::state::mods::manifest {

/**
 * Reads the bounded configBindings array without changing declarations.
 * @param json Complete array text.
 * @param output Receives parsed sites.
 * @param error Receives the first refusal.
 * @return False on malformed, duplicate or unsupported site fields.
 */
[[nodiscard]] bool read_bindings(std::string_view json, Manifest& output, Error& error) noexcept;

/**
 * Checks declaration types, source identities and non-overlapping scalar sites.
 * @param manifest Fully parsed manifest.
 * @param error Receives the first refusal.
 * @return True when every site names a compatible declared key.
 */
[[nodiscard]] bool check_bindings(const Manifest& manifest, Error& error) noexcept;

} // namespace sunrise::state::mods::manifest
