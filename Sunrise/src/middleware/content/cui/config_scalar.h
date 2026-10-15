#pragma once

#include <cstddef>
#include <span>

#include "../../../state/mods/definition.h"

namespace sunrise::middleware::content::cui {

/**
 * Validates isolated typed scalar sites in one CUI resource.
 * @param bytes Exact entry bytes whose digest the package stage has checked.
 * @param sites Non-overlapping sites for this one entry.
 * @return False on unsupported ownership, storage, aliasing or malformed arrays.
 */
[[nodiscard]] bool
validate_config_scalars(std::span<const std::byte> bytes,
                        std::span<const state::mods::ConfigBinding> sites) noexcept;

} // namespace sunrise::middleware::content::cui
