#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../definition.h"

namespace sunrise::state::mods::resolution {

/** Record indices in load order. */
using Order = std::array<std::size_t, kModCapacity>;

/**
 * Decides which mods load and in what order, and records every refusal on its mod.
 * A mod loads after every mod it depends on. Mods with no order between them go by id.
 * @param records Every discovered mod. A parsed manifest arrives with status loaded.
 * @param order Receives record indices in load order.
 * @return The number of mods in `order`.
 */
[[nodiscard]] std::size_t resolve(std::span<Record> records, Order& order) noexcept;

} // namespace sunrise::state::mods::resolution
