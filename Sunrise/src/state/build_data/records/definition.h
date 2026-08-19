#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::records {

/** The shipped build declares 2242 records and lore entries. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 4096;

/** A record whose completion flag no mapping table addresses carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableFlagIndex = 0xFFFFU;

/**
 * One record reduced to what a claim needs.
 *
 * A record row carries the unlock slot of its completion flag. A slot is not an array index: the
 * byte that feeds it lives at the row number of the mapping table whose destination is that slot.
 * The index is resolved once here, at extraction, so a claim never has to walk the mapping tables.
 */
struct Definition {
    /** Native record row, which is what an opcode-1801 claim names. */
    std::uint16_t definitionIndex{};
    /** Account flag bank mapping row, or kUnavailableFlagIndex when the slot is unaddressable. */
    std::uint16_t completionFlagIndex{kUnavailableFlagIndex};
};

} // namespace sunrise::state::build_data::records
