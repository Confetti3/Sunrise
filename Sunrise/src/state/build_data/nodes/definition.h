#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::nodes {

/** The shipped build declares 924 presentation nodes. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 1024;

/** No shipped node owns more records than this; the widest seen is a lore book at fifteen. */
inline constexpr std::size_t kChildCapacity = 64;

/** A node whose expression names no addressable value slot carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableValueIndex = 0xFFFFU;

/**
 * One presentation node reduced to what a progress bar needs.
 *
 * A node's bar is not derived by the client from its children. The node carries an expression that
 * reads a value slot, and the bar shows whatever that slot holds, so a server that wants the bar to
 * move has to count the claimed children itself and write the count. The slot is resolved to its
 * mapping-table row here, at extraction, exactly as a record's completion flag is.
 */
struct Definition {
    /** Native node row. */
    std::uint16_t definitionIndex{};
    /** Account value bank mapping row, or kUnavailableValueIndex when no slot is addressable. */
    std::uint16_t valueIndex{kUnavailableValueIndex};
    /** Records this node owns, held at node row `+136`. */
    std::uint8_t childCount{};
    /** Native record rows of the owned records. */
    std::array<std::uint16_t, kChildCapacity> children{};
};

} // namespace sunrise::state::build_data::nodes
