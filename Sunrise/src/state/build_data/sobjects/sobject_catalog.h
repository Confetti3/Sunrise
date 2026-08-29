#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::sobjects {

/**
 * The definition table an incident target names.
 *
 * An incident is the client's one general typed gameplay event, and its target is a 13-bit index
 * into this table. Pickup handling resolves that target to identify an exact record where the
 * installed row provides one, or to select the appropriate fallback path.
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
    /** The eight opaque 32-bit lanes preceding the selector and type fields. */
    std::array<std::uint32_t, 8> lanes{};
    /** FNV-1 of the definition name. */
    std::uint32_t nameHash{};
    /** Selects the payload shape. Runs -1 to 48. */
    std::int32_t typeCode{kAbsentTypeCode};
    /** Compressed-selector group, or 0xFFFF for none. */
    std::uint16_t selectorGroup{};
    /** Position in that group's node table. */
    std::uint16_t nodeOrdinal{};
    /**
     * Row offset +16, "lane 4": a packed pair of u16s whose meaning is chosen by typeCode, not
     * fixed. On typeCode 10 (dead ghosts and similar) the low half is a DestinyRecordDefinition row
     * index -- 807 rows carry this type, 360 of them resolving. On type-code 2 world lore objects,
     * the high half is a lore-object ordinal rather than a DestinyCollectibleDefinition row. The
     * installed crystal and bone runs are contiguous and follow their manifest chapter order.
     */
    std::uint32_t lane4{};

    /** @return Lane 4's low half, the record row a typeCode-10 sobject names. */
    [[nodiscard]] constexpr std::uint16_t recordRow() const noexcept {
        return static_cast<std::uint16_t>(lane4 & 0xFFFFU);
    }

    /** @return Lane 4's high half, the world-lore ordinal a type-code 2 sobject names. */
    [[nodiscard]] constexpr std::uint16_t loreObjectOrdinal() const noexcept {
        return static_cast<std::uint16_t>(lane4 >> 16U);
    }

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
