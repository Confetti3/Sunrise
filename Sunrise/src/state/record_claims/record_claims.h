#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::record_claims {

/**
 * Records claimed through Web Service opcode 1801, as account flag bank indices.
 *
 * The authored unlock policy is immutable for the life of the process, so a claim cannot write to
 * it. This holds the claims instead, and the account encoder lays them over the authored bank on
 * its way out. A claim is therefore visible to the client on the next Family-4 image.
 *
 * Claims are written to a file beside the build data cache, so they survive a restart. Settings
 * stay configuration: nothing here edits them.
 */

/**
 * Derives the claim file path and loads any claims already held.
 * A missing file is not a failure: it is an account that has claimed nothing yet.
 * @param module Loaded DLL, used to find the artifact directory.
 * @return True when the path resolves. Loading is best effort and reported separately.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Forgets every held claim, in memory only. The file is left alone. */
void clear() noexcept;

/**
 * Marks one account flag bank index claimed, adds its score, and writes the claim file.
 * A repeated claim of the same index is held once, scores once, and rewrites nothing.
 * @param flagIndex Mapping-table row whose object byte feeds the record's completion flag.
 * @param scoreValue Points the record is worth, counted only on the first claim.
 * @return True when the index is in range and the claim is now held.
 */
[[nodiscard]] bool claim(std::uint16_t flagIndex, std::uint16_t scoreValue) noexcept;

/**
 * Marks one record complete but unclaimed, so the client offers it as claimable.
 *
 * Finding lore completes a triumph; claiming it is a separate act by the player. A record marked
 * this way and later claimed carries the claim instead, so the two never conflict.
 * @param flagIndex Mapping-table row whose object byte feeds the record's completion flag.
 * @return True when the index is in range and the record is now marked.
 */
[[nodiscard]] bool mark_claimable(std::uint16_t flagIndex) noexcept;

/** @return True when this index is marked claimable, whether or not it is also claimed. */
[[nodiscard]] bool claimable(std::uint16_t flagIndex) noexcept;

/**
 * Lays every held claim over one account flag bank.
 * @param accountFlags Bank already filled from the authored policy.
 * @return Number of bytes this changed, so a caller can tell a no-op from real work.
 */
std::size_t apply(std::span<std::uint8_t> accountFlags) noexcept;

/**
 * Writes each presentation node's claimed-child count into the value slot its bar reads.
 *
 * A node's progress bar is not derived by the client from its children: the node names a value slot
 * and shows whatever it holds. Counting here is what makes claiming a chapter move its book.
 * @param objectiveValues Account value bank, already filled from the authored policy.
 * @return Number of nodes whose slot was written.
 */
std::size_t apply_node_progress(std::span<std::int32_t> objectiveValues) noexcept;

/**
 * Writes each claimable-and-unclaimed record's authored objective value(s) into the objective
 * value bank, so its triumph reads at completionValue while its completion flag stays clear --
 * the two conditions the client requires before it will offer a claim. The flag itself is never
 * touched here: writing it can only mean claimed or nothing, never claimable, so claimable is
 * carried by the objective bank alone.
 * @param objectiveValues Account value bank, already filled from the authored policy.
 * @return Number of values this wrote.
 */
std::size_t apply_claimable_objectives(std::span<std::int32_t> objectiveValues) noexcept;

/**
 * Publishes the per-chapter visibility gate of the Year 1 lore chapters.
 *
 * A lore chapter is displayed only when a value slot of its own holds at least the chapter's
 * completion value; below that the client shows a redacted entry. The slot is the record's own row
 * offset into a contiguous block sitting immediately above the Year 1 parent bars -- measured in
 * game, see kChapterGateBase -- and the test is a threshold, not an equality, which is why probing
 * the block with a flat 1 lit every chapter that completes at 1 and no other.
 *
 * Only the chapters below kChapterGateLastRow need this. Every later chapter is displayed by
 * default, and the same arithmetic would put their slots inside the record-objective range, where
 * writing has previously redacted records wholesale.
 * @param objectiveValues Account value bank.
 * @return Number of gates published.
 */
std::size_t apply_chapter_visibility_gates(std::span<std::int32_t> objectiveValues) noexcept;

/** @return True when this index is already held. */
[[nodiscard]] bool claimed(std::uint16_t flagIndex) noexcept;

/**
 * Writes each category's claimed-child count into the character value slot its bar reads.
 * One book counts in the character bank rather than the account one.
 * @param characterValues Character value bank, already filled from the authored policy.
 * @return Number of categories written.
 */
std::size_t apply_character_node_progress(std::span<std::int32_t> characterValues) noexcept;

/** @return Total score of every held claim. */
[[nodiscard]] std::uint32_t total_score() noexcept;

/** @return Number of distinct indices held. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::record_claims
