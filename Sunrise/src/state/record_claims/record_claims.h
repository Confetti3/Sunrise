#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::record_claims {

/**
 * Records claimed through Web Service opcode 1801, as account flag bank indices.
 *
 * The authored unlock policy is immutable for the life of the process, so a claim cannot write to
 * it. This holds the claims made since boot instead, and the account encoder lays them over the
 * authored bank on its way out. A claim is therefore visible to the client on the next Family-4
 * image, and is lost on restart unless the same flag is authored.
 */

/** Forgets every claim made since boot. */
void clear() noexcept;

/**
 * Marks one account flag bank index claimed and adds its record's score to the total.
 * A repeated claim of the same index is held once and scores once.
 * @param flagIndex Mapping-table row whose object byte feeds the record's completion flag.
 * @param scoreValue Points the record is worth, counted only on the first claim.
 * @return True when the index is in range and the claim is now held.
 */
[[nodiscard]] bool claim(std::uint16_t flagIndex, std::uint16_t scoreValue) noexcept;

/** @return Total score of every record claimed since boot. */
[[nodiscard]] std::uint32_t total_score() noexcept;

/**
 * Lays every held claim over one account flag bank.
 * @param accountFlags Bank already filled from the authored policy.
 * @return Number of bytes this changed, so a caller can tell a no-op from real work.
 */
std::size_t apply(std::span<std::uint8_t> accountFlags) noexcept;

/** @return Number of distinct indices claimed since boot. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::record_claims
