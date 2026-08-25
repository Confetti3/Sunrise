#include "lore_grant.h"

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
#include "../build_data/runtime.h"
#include "../record_claims/record_claims.h"
#include "bubble_record_table.h"

namespace sunrise::state::lore {
namespace {

std::atomic<std::uint16_t> g_lastGranted{0};
std::atomic<bool> g_lastItemGranted{false};

/**
 * Publishes the node and record tables once, so a warm start does not run either path empty.
 *
 * On a warm start the package pass is skipped, so neither table is published until something asks
 * for it. Both grant paths need the record table and the book path also needs the node table, so
 * both are brought up together here; a caller that only needs records still pays for nodes once,
 * which costs nothing once the tables are up and is simpler than tracking each domain apart.
 * @return True once both tables are known published, this call or an earlier one.
 */
bool ensure_tables_published() noexcept {
    namespace nodes = build_data::nodes;
    namespace records = build_data::records;
    static std::atomic<bool> published{false};
    if (published.load(std::memory_order_relaxed)) {
        return true;
    }
    const bool haveNodes = nodes::count() != 0 || nodes::load_and_publish();
    const bool haveRecords = records::count() != 0 || records::load_and_publish();
    if (haveNodes && haveRecords) {
        published.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

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
    case GrantOutcome::recordNotFound:
        return "record_not_found";
    case GrantOutcome::noFlag:
        return "no_flag";
    case GrantOutcome::alreadyHeld:
        return "already_held";
    case GrantOutcome::notAChapter:
        return "not_a_chapter";
    case GrantOutcome::bubbleTableExhausted:
        return "bubble_table_exhausted";
    }
    return "unknown";
}

/** Grants the next chapter of one book that the account does not already hold. */
GrantOutcome grant_next_chapter(std::uint16_t node) noexcept {
    if (node == kNoBook) {
        return GrantOutcome::unknownBook;
    }

    namespace nodes = build_data::nodes;
    namespace records = build_data::records;
    (void)ensure_tables_published();
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

/** Grants one record's completion directly, by the row an sobject's lane 4 names. */
GrantOutcome grant_record(std::uint16_t definitionIndex) noexcept {
    namespace records = build_data::records;
    (void)ensure_tables_published();

    records::Definition record{};
    if (!records::find(definitionIndex, record)) {
        return GrantOutcome::recordNotFound;
    }
    if (record.completionFlagIndex == records::kUnavailableFlagIndex) {
        return GrantOutcome::noFlag;
    }
    // Same rule as grant_next_chapter: a record naming no lore row is not a chapter. Here it also
    // means lane 4 did not really name a record -- see GrantOutcome::notAChapter -- so the caller
    // is told to fall back rather than granting whatever triumph the number happened to land on.
    if (record.loreRow == records::kUnavailableLoreRow) {
        return GrantOutcome::notAChapter;
    }
    // Same rule as grant_next_chapter: finding lore completes a chapter, claiming it is the
    // player's own act, so a record already claimed or already offered claimable is left alone
    // rather than reported as this pickup's doing.
    if (record_claims::claimed(record.completionFlagIndex)
        || record_claims::claimable(record.completionFlagIndex)) {
        return GrantOutcome::alreadyHeld;
    }
    if (!record_claims::mark_claimable(record.completionFlagIndex)) {
        return GrantOutcome::refused;
    }
    g_lastGranted.store(definitionIndex, std::memory_order_relaxed);
    return GrantOutcome::granted;
}

std::uint16_t legacy_book_for_bubble(std::uint32_t bubble) noexcept {
    // Thieves' Landing. Its checklist entries are Region Chests, so the Ghost Lore join that built
    // bubble_record_table.h never produced a bucket for it, but a pickup here does grant a Ghost
    // Stories chapter -- observed granting record 802 before the table replaced kBubbleBooks.
    constexpr std::uint32_t kThievesLandingBubble = 0x5FE28198U;

    if (bubble == kThievesLandingBubble) {
        return kGhostStoriesNode;
    }
    return kNoBook;
}

/**
 * Grants the first record in one bubble's generated candidate bucket that the account does not
 * already hold, walking the bucket in table order.
 */
GrantOutcome grant_from_bubble_table(std::uint32_t bubble, std::size_t& bucketSize) noexcept {
    const std::span<const std::uint16_t> rows = bubble_record_table::records_for_bubble(bubble);
    bucketSize = rows.size();
    if (rows.empty()) {
        return GrantOutcome::unknownBook;
    }

    // Each row is tried through grant_record, which is the one place that already knows how to
    // check and write a claim -- there is no second mechanism to maintain here. A row this bucket
    // names but that is already held is exactly the case a later pickup in the same bubble is
    // meant to skip past, so the walk continues; a row that fails to resolve at all would be a
    // data problem in the generated table, and is skipped rather than aborting the whole pickup.
    // Only a claim-store refusal stops the walk outright, the same as grant_next_chapter.
    for (const std::uint16_t row : rows) {
        const GrantOutcome outcome = grant_record(row);
        if (outcome == GrantOutcome::granted || outcome == GrantOutcome::refused) {
            return outcome;
        }
    }
    return GrantOutcome::bubbleTableExhausted;
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
