#pragma once

#include <cstddef>
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
 * So the chapter is chosen here instead. A pickup grants the next chapter its bubble has candidates
 * for that the account does not already hold, which is what a player observes anyway: collect, and
 * the book fills in. The association needed is bubble -> candidate chapters (bubble_record_table,
 * generated from the manifest) rather than one reward per object -- except for Confessions, whose
 * caluseum_experience activity is instanced and so carries no bubble at all; see kConfessionsNode.
 */

/** A node index that names no book. */
inline constexpr std::uint16_t kNoBook = 0xFFFFU;

/**
 * The FNV-1a 32-bit offset basis -- the hash of the empty string.
 *
 * Every instanced activity reports this value when its bubble field is otherwise unset. It is not
 * an authored bubble, is confirmed absent from the whole manifest, and must never be looked up in
 * bubble_record_table (which asserts as much at compile time). A bubble equal to this is the
 * instanced-activity case: see kConfessionsNode.
 */
inline constexpr std::uint32_t kBubbleUnsetSentinel = 0x811C9DC5U;

/**
 * Presentation node of Confessions, the caluseum_experience book whose vases feed it.
 *
 * Confessions has no bubble: caluseum_experience is instanced, so every one of its pickups reports
 * 0x811C9DC5, the FNV-1a 32-bit offset basis every instanced activity carries when its bubble field
 * is otherwise unset -- not an authored identifier for this or any other place. It is therefore
 * never looked up in the generated bubble table (see bubble_record_table.h, which asserts the
 * sentinel absent); the caller recognises the sentinel directly and grants against this node
 * instead, the same ordered walk grant_next_chapter always did here. Node 838 confirmed against the
 * published manifest: nine chapters, Entry I on record 1708 carrying lore hash 0x58C9C088.
 */
inline constexpr std::uint16_t kConfessionsNode = 838U;

/**
 * Presentation node of Ghost Stories. Confirmed against the published manifest: 24 chapters.
 *
 * Ghost Stories is granted by Dead Ghosts, which are scattered across the whole game rather than
 * gathered into one destination, so the generated bubble table does not cover every bubble that
 * hosts one -- the public map data it is built from simply never recorded them. Bubbles known to
 * host a pickup but absent from the table fall back to an ordered walk of this node, which is what
 * the retired kBubbleBooks did for them. It grants the right book and an arbitrary chapter of it,
 * which beats granting nothing; see legacy_book_for_bubble.
 */
inline constexpr std::uint16_t kGhostStoriesNode = 817U;

/**
 * Book to walk for a bubble the generated table does not cover, or kNoBook when none is known.
 *
 * This is deliberately a short hand-maintained list and not a second generated table: it exists
 * only to stop a pickup in a known-good bubble granting nothing while that bubble is missing from
 * bubble_record_map.json. Entries should be deleted as the generated table grows to cover them.
 */
[[nodiscard]] std::uint16_t legacy_book_for_bubble(std::uint32_t bubble) noexcept;

/** Why one pickup granted nothing. */
enum class GrantOutcome : std::uint8_t {
    granted,
    /** The bubble is not associated with a book. */
    unknownBook,
    /** The node table is not published, so no book can be read. */
    noNodeTable,
    /** The node table is published but does not contain this book. */
    bookNotFound,
    /** The book is present but owns no children. */
    noChildren,
    /** The children are present but no record resolves, so the record table is missing. */
    noRecords,
    /** Records resolve but none of them is a chapter. */
    noChapters,
    /** Every chapter of the book is already held. */
    bookComplete,
    /** The claim store refused the write. */
    refused,
    /** The named record row does not exist, or the record table is not published. */
    recordNotFound,
    /** The record exists but names no completion flag, so it cannot be claimed. */
    noFlag,
    /** The record is already claimed or already marked claimable. */
    alreadyHeld,
    /**
     * The named record is not a lore chapter, so this pickup is not what completes it.
     *
     * A type-10 row's lane 4 resolves into the record table for 360 of 807 rows, but only 102 of
     * those name a record that carries a lore row. The rest land on unrelated triumphs -- Season of
     * Dawn, Titan Strength, Reward: Jumpship -- because a small u16 falls inside a 2242 row table
     * often enough to look like an index without being one. Granting those would hand out a
     * visibly wrong triumph, so they are refused here and left to the bubble table / fallback.
     */
    notAChapter,
    /**
     * The bubble resolved to a bucket in the generated table, but every candidate row in it is
     * already claimed or claimable. The caller's cue to fall through to the fallback path rather
     * than report a silent no-op.
     */
    bubbleTableExhausted,
};

/** @return A short name for the outcome, for logs. */
[[nodiscard]] const char* grant_outcome_name(GrantOutcome outcome) noexcept;

/**
 * Grants the first record in one bubble's generated candidate bucket that the account does not
 * already hold, walking the bucket in table order.
 *
 * Replaces the old kBubbleBooks heuristic (one book per activity, guessed at) with the generated
 * bubble_record_table (one set of candidate chapters per bubble, joined from the manifest). Where a
 * bucket holds several rows, successive pickups in that bubble hand out successive chapters -- this
 * does not claim to identify the exact physical object, only to walk the same small set a player
 * would see filling in as they collect.
 *
 * @param bubble Bubble hash the incident carried. Never pass kBubbleUnsetSentinel here -- the
 *        caller must recognise that case itself and use kConfessionsNode with grant_next_chapter
 *        instead; bubble_record_table asserts the sentinel is not one of its entries.
 * @param bucketSize Out: number of candidate rows the bubble's bucket held, 0 when the bubble is not
 *        in the table. For logs.
 * @return granted; unknownBook when the bubble is not in the generated table; bubbleTableExhausted
 *         when it is but every candidate is already held; or whatever grant_record returned for the
 *         row that stopped the walk (recordNotFound, noFlag, notAChapter, refused).
 */
[[nodiscard]] GrantOutcome grant_from_bubble_table(std::uint32_t bubble, std::size_t& bucketSize) noexcept;

/**
 * Grants the next chapter of one book that the account does not already hold.
 * Chapters are the node's children that name a lore row; the child naming none is the book's parent
 * triumph and is never granted.
 * @param node Presentation node of the book.
 * @return What happened, so a refusal reads differently from a completed book.
 */
[[nodiscard]] GrantOutcome grant_next_chapter(std::uint16_t node) noexcept;

/**
 * Grants one record's completion directly, by the row an sobject's lane 4 names.
 *
 * Performs the same write grant_next_chapter does -- record_claims::mark_claimable on the record's
 * completion flag -- but on a row named exactly rather than one found by walking a book's children.
 * This is the path a type-10 incident takes when its target's lane 4 resolves a record on its own,
 * which makes guessing at the book unnecessary.
 * @param definitionIndex Native record row, from an sobject's lane 4 low half.
 * @return What happened. recordNotFound means the row itself does not resolve -- the caller's cue
 *         to fall back to the bubble table / instanced case instead of treating this as a completed
 *         attempt.
 */
[[nodiscard]] GrantOutcome grant_record(std::uint16_t definitionIndex) noexcept;

/** @return The record row the last successful grant claimed. Only meaningful after `granted`. */
[[nodiscard]] std::uint16_t last_granted_record() noexcept;

/** @return True when the last grant also gave the collectible's item, which the lore is gated on. */
[[nodiscard]] bool last_item_granted() noexcept;

} // namespace sunrise::state::lore
