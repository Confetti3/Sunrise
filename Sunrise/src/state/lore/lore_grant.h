#pragma once

#include <cstdint>

namespace sunrise::state::lore {

/**
 * Granting a lore chapter when a collectible is picked up.
 *
 * The incident a pickup emits does not name what it granted. Its only per-object content is a
 * position: the same object picked twice produces a payload differing by one bit, the sequence,
 * while a different object differs only across the position vector. Reproducing Bungie's choice of
 * chapter would mean mapping every object in the world to a reward, from activity and spawn data.
 *
 * So the chapter is chosen here instead. A pickup grants the next chapter of its book that the
 * account does not already hold, which is what a player observes anyway: collect, and the book
 * fills in. The association needed is one book per activity rather than one reward per object.
 */

/** A node index that names no book. */
inline constexpr std::uint16_t kNoBook = 0xFFFFU;

/** Why one pickup granted nothing. */
enum class GrantOutcome : std::uint8_t {
    granted,
    /** The bubble is not associated with a book. */
    unknownBook,
    /** The book has no chapters this build can resolve. */
    emptyBook,
    /** Every chapter of the book is already held. */
    bookComplete,
    /** The claim store refused the write. */
    refused,
};

/** @return A short name for the outcome, for logs. */
[[nodiscard]] const char* grant_outcome_name(GrantOutcome outcome) noexcept;

/**
 * @param bubble Bubble hash the incident carried.
 * @return The presentation node of the book that activity's pickups feed, or kNoBook.
 */
[[nodiscard]] std::uint16_t book_for_bubble(std::uint32_t bubble) noexcept;

/**
 * Grants the next chapter of one book that the account does not already hold.
 * Chapters are the node's children that name a lore row; the child naming none is the book's parent
 * triumph and is never granted.
 * @param node Presentation node of the book.
 * @return What happened, so a refusal reads differently from a completed book.
 */
[[nodiscard]] GrantOutcome grant_next_chapter(std::uint16_t node) noexcept;

/** @return The record row the last successful grant claimed. Only meaningful after `granted`. */
[[nodiscard]] std::uint16_t last_granted_record() noexcept;

} // namespace sunrise::state::lore
