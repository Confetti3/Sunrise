#include "lore_grant.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "../build_data/nodes/definition.h"
#include "../build_data/nodes/node_catalog.h"
#include "../build_data/nodes/node_persistence.h"
#include "../build_data/records/definition.h"
#include "../build_data/records/record_catalog.h"
#include "../build_data/records/record_persistence.h"
#include "../build_data/collectibles/collectible_catalog.h"
#include "../build_data/inventory/buckets/definition.h"
#include "../build_data/items/details/definition.h"
#include "../runtime/runtime.h"
#include "../build_data/runtime.h"
#include "../record_claims/record_claims.h"

namespace sunrise::state::lore {
namespace {

/**
 * Which book an activity's pickups feed.
 *
 * One entry per activity, not one per object. This is the whole of the authored data the chosen
 * design needs, and it is the part that would be replaced if Bungie's own object-to-reward mapping
 * were ever reproduced from activity and spawn data.
 *
 * Only the Menagerie is known so far, measured from its pickups: the bubble its incidents carry
 * against the book those pickups fill.
 */
struct BubbleBook {
    std::uint32_t bubble;
    std::uint16_t node;
};

constexpr std::array<BubbleBook, 2> kBubbleBooks{{
    // caluseum_experience, whose vases fill Confessions. Node 838 confirmed against the published
    // manifest: nine chapters, Entry I on record 1708 carrying lore hash 0x58C9C088.
    {0x811C9DC5U, 838U},
    // The destination whose dead ghosts fill Ghost Stories. Node 817 identified by child count --
    // the manifest lists 23 records and only two books here have 23 chapters -- and by section,
    // since Ghost Stories sits under The Light and the other candidate, node 847, does not.
    {0x5FE28198U, 817U},
}};

std::atomic<std::uint16_t> g_lastGranted{0};
std::atomic<bool> g_lastItemGranted{false};

} // namespace

/** @return A short name for the outcome, for logs. */
const char* grant_outcome_name(GrantOutcome outcome) noexcept {
    switch (outcome) {
    case GrantOutcome::granted:
        return "granted";
    case GrantOutcome::unknownBook:
        return "unknown_book";
    case GrantOutcome::noNodeTable:
        return "no_node_table";
    case GrantOutcome::bookNotFound:
        return "book_not_found";
    case GrantOutcome::noChildren:
        return "no_children";
    case GrantOutcome::noRecords:
        return "no_records";
    case GrantOutcome::noChapters:
        return "no_chapters";
    case GrantOutcome::bookComplete:
        return "book_complete";
    case GrantOutcome::refused:
        return "refused";
    }
    return "unknown";
}

/** @return The presentation node of the book that activity's pickups feed. */
std::uint16_t book_for_bubble(std::uint32_t bubble) noexcept {
    for (const BubbleBook& entry : kBubbleBooks) {
        if (entry.bubble == bubble) {
            return entry.node;
        }
    }
    return kNoBook;
}

/** Grants the next chapter of one book that the account does not already hold. */
GrantOutcome grant_next_chapter(std::uint16_t node) noexcept {
    if (node == kNoBook) {
        return GrantOutcome::unknownBook;
    }

    namespace nodes = build_data::nodes;
    namespace records = build_data::records;
    // On a warm start the package pass is skipped, so the node table is never published and every
    // book looks empty. The table is kept in its own file for exactly this; publishing it here
    // costs nothing once it has succeeded.
    static std::atomic<bool> published{false};
    if (!published.load(std::memory_order_relaxed)) {
        const bool haveNodes = nodes::count() != 0 || nodes::load_and_publish();
        const bool haveRecords = records::count() != 0 || records::load_and_publish();
        if (haveNodes && haveRecords) {
            published.store(true, std::memory_order_relaxed);
        }
    }
    std::vector<nodes::Definition> rows(nodes::kDefinitionCapacity);
    std::size_t count = 0;
    if (!nodes::snapshot(std::span<nodes::Definition>{rows}, count) || count == 0) {
        return GrantOutcome::noNodeTable;
    }

    const nodes::Definition* book = nullptr;
    for (std::size_t row = 0; row < count; ++row) {
        if (rows[row].definitionIndex == node) {
            book = &rows[row];
            break;
        }
    }
    if (book == nullptr) {
        return GrantOutcome::bookNotFound;
    }
    if (book->childCount == 0) {
        return GrantOutcome::noChildren;
    }

    bool sawRecord = false;
    bool sawChapter = false;
    for (std::size_t child = 0; child < book->childCount; ++child) {
        records::Definition record{};
        if (!build_data::find_record_definition(book->children[child], record)) {
            continue;
        }
        sawRecord = true;
        if (record.completionFlagIndex == records::kUnavailableFlagIndex) {
            continue;
        }
        // A child naming no lore row is the book's parent triumph, not a chapter. Granting it would
        // mark the book complete without giving any of its contents.
        if (record.loreRow == records::kUnavailableLoreRow) {
            continue;
        }
        sawChapter = true;
        // Finding lore completes a chapter; claiming it is the player's act, not this one. So the
        // record is left claimable rather than claimed, and a chapter already in either state is
        // passed over.
        if (record_claims::claimed(record.completionFlagIndex)
            || record_claims::claimable(record.completionFlagIndex)) {
            continue;
        }
        if (!record_claims::mark_claimable(record.completionFlagIndex)) {
            return GrantOutcome::refused;
        }
        g_lastGranted.store(record.definitionIndex, std::memory_order_relaxed);
        return GrantOutcome::granted;
    }
    if (sawChapter) {
        return GrantOutcome::bookComplete;
    }
    return sawRecord ? GrantOutcome::noChapters : GrantOutcome::noRecords;
}

/** @return True when the last grant also gave the collectible's item. */
bool last_item_granted() noexcept {
    return g_lastItemGranted.load(std::memory_order_relaxed);
}

/** @return The record row the last successful grant claimed. */
std::uint16_t last_granted_record() noexcept {
    return g_lastGranted.load(std::memory_order_relaxed);
}

} // namespace sunrise::state::lore
