#include "lore_grant.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "../build_data/nodes/definition.h"
#include "../build_data/nodes/node_catalog.h"
#include "../build_data/records/definition.h"
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

constexpr std::array<BubbleBook, 1> kBubbleBooks{{
    // caluseum_experience, whose vases fill Confessions.
    {0x811C9DC5U, 838U},
}};

std::atomic<std::uint16_t> g_lastGranted{0};

} // namespace

/** @return A short name for the outcome, for logs. */
const char* grant_outcome_name(GrantOutcome outcome) noexcept {
    switch (outcome) {
    case GrantOutcome::granted:
        return "granted";
    case GrantOutcome::unknownBook:
        return "unknown_book";
    case GrantOutcome::emptyBook:
        return "empty_book";
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
    std::vector<nodes::Definition> rows(nodes::kDefinitionCapacity);
    std::size_t count = 0;
    if (!nodes::snapshot(std::span<nodes::Definition>{rows}, count)) {
        return GrantOutcome::emptyBook;
    }

    const nodes::Definition* book = nullptr;
    for (std::size_t row = 0; row < count; ++row) {
        if (rows[row].definitionIndex == node) {
            book = &rows[row];
            break;
        }
    }
    if (book == nullptr || book->childCount == 0) {
        return GrantOutcome::emptyBook;
    }

    bool sawChapter = false;
    for (std::size_t child = 0; child < book->childCount; ++child) {
        records::Definition record{};
        if (!build_data::find_record_definition(book->children[child], record)
            || record.completionFlagIndex == records::kUnavailableFlagIndex) {
            continue;
        }
        // A child naming no lore row is the book's parent triumph, not a chapter. Granting it would
        // mark the book complete without giving any of its contents.
        if (record.loreRow == records::kUnavailableLoreRow) {
            continue;
        }
        sawChapter = true;
        if (record_claims::claimed(record.completionFlagIndex)) {
            continue;
        }
        if (!record_claims::claim(record.completionFlagIndex, record.scoreValue)) {
            return GrantOutcome::refused;
        }
        g_lastGranted.store(record.definitionIndex, std::memory_order_relaxed);
        return GrantOutcome::granted;
    }
    return sawChapter ? GrantOutcome::bookComplete : GrantOutcome::emptyBook;
}

/** @return The record row the last successful grant claimed. */
std::uint16_t last_granted_record() noexcept {
    return g_lastGranted.load(std::memory_order_relaxed);
}

} // namespace sunrise::state::lore
