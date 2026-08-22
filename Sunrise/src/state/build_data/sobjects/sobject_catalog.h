#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::sobjects {

/**
 * The definition table an incident target names.
 *
 * An incident is the client's one general typed gameplay event, and its target is a 13-bit index
 * into this table. Nothing acts on an incident today, so a collectible picked up in the world tells
 * the server nothing. Resolving the target is the first step of acting on one.
 *
 * The table is shipped package data. Its class is `0x80807C9B` and two installed tags carry it with
 * byte-identical hash and type-code columns, so a row index means the same thing against either.
 */

/** The wire target is thirteen bits, and 7763 rows install. */
inline constexpr std::size_t kDefinitionCapacity = 8192;

/** A row carrying this type code has no handler and cannot produce a schema. */
inline constexpr std::int32_t kAbsentTypeCode = -1;

/** One row of the table, reduced to what resolving a target needs. */
struct Definition {
    /** FNV-1 of the definition name. */
    std::uint32_t nameHash{};
    /** Selects the payload shape. Runs -1 to 48. */
    std::int32_t typeCode{kAbsentTypeCode};
    /** Compressed-selector group, or 0xFFFF for none. */
    std::uint16_t selectorGroup{};
    /** Position in that group's node table. */
    std::uint16_t nodeOrdinal{};
};

/** Clears every generated row. */
void clear() noexcept;

/** @return True when the rows are dense and within capacity. */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/** Replaces the whole table in one step. */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one row by the target index an incident carries.
 * @param targetIndex Index from the wire, already range-checked by the incident validator.
 * @param definition Receives the row.
 * @return True when the index is in range.
 */
[[nodiscard]] bool find(std::uint16_t targetIndex, Definition& definition) noexcept;

/** @return Number of installed rows. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::sobjects
