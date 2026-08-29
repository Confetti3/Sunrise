#pragma once

#include <cstdint>

namespace sunrise::state::lore {

/** Why one exact collectible grant did not change account state. */
enum class GrantOutcome : std::uint8_t {
    granted,
    /** A counted record advanced but remains below its completion value. */
    progressed,
    refused,
    recordNotFound,
    noFlag,
    alreadyHeld,
    /** The named record is not a lore chapter. */
    notAChapter,
    /** The SObject's collectible row was not published. */
    collectibleNotFound,
    /** The collectible does not unlock a lore row. */
    collectibleNoLore,
    /** No chapter record displays the collectible's lore row. */
    loreRecordNotFound,
};

/** @return A short name for the outcome, for logs. */
[[nodiscard]] const char* grant_outcome_name(GrantOutcome outcome) noexcept;

/** Grants one exact lore record by its native definition row. */
[[nodiscard]] GrantOutcome grant_record(std::uint16_t definitionIndex) noexcept;

/** Advances one exact counted lore record by one objective unit. */
[[nodiscard]] GrantOutcome advance_record(std::uint16_t definitionIndex) noexcept;

/** Resolves a type-2 SObject collectible row to its exact lore record and grants it. */
[[nodiscard]] GrantOutcome grant_collectible(std::uint16_t collectibleIndex) noexcept;

/** @return The record row the last successful grant or progress advance changed. */
[[nodiscard]] std::uint16_t last_granted_record() noexcept;

} // namespace sunrise::state::lore
