#include "lore_grant.h"

#include <atomic>
#include "../build_data/collectibles/collectible_catalog.h"
#include "../build_data/records/definition.h"
#include "../build_data/records/record_catalog.h"
#include "../build_data/records/record_persistence.h"
#include "../build_data/runtime.h"
#include "../record_claims/record_claims.h"

namespace sunrise::state::lore {
namespace {

std::atomic<std::uint16_t> g_lastGranted{0};

/** Publishes the record table once, so a warm start does not resolve exact rows empty. */
bool ensure_records_published() noexcept {
    namespace records = build_data::records;
    static std::atomic<bool> published{false};
    if (published.load(std::memory_order_relaxed)) {
        return true;
    }
    const bool haveRecords = records::count() != 0 || records::load_and_publish();
    if (haveRecords) {
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
    case GrantOutcome::progressed:
        return "progressed";
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
    case GrantOutcome::collectibleNotFound:
        return "collectible_not_found";
    case GrantOutcome::collectibleNoLore:
        return "collectible_no_lore";
    case GrantOutcome::loreRecordNotFound:
        return "lore_record_not_found";
    }
    return "unknown";
}

/** Grants one record's completion directly, by the row an sobject's lane 4 names. */
GrantOutcome grant_record(std::uint16_t definitionIndex) noexcept {
    namespace records = build_data::records;
    (void)ensure_records_published();

    records::Definition record{};
    if (!records::find(definitionIndex, record)) {
        return GrantOutcome::recordNotFound;
    }
    if (record.completionFlagIndex == records::kUnavailableFlagIndex) {
        return GrantOutcome::noFlag;
    }
    // A record naming no lore row is not a chapter and must never be granted by a collectible.
    if (record.loreRow == records::kUnavailableLoreRow) {
        return GrantOutcome::notAChapter;
    }
    // Finding lore completes a chapter; claiming it is the player's act, so an already-held
    // chapter is left alone rather than reported as this pickup's doing.
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

/** Advances one counted chapter record without completing it early. */
GrantOutcome advance_record(std::uint16_t definitionIndex) noexcept {
    namespace records = build_data::records;
    (void)ensure_records_published();

    records::Definition record{};
    if (!records::find(definitionIndex, record)) {
        return GrantOutcome::recordNotFound;
    }
    if (record.completionFlagIndex == records::kUnavailableFlagIndex) {
        return GrantOutcome::noFlag;
    }
    if (record.loreRow == records::kUnavailableLoreRow) {
        return GrantOutcome::notAChapter;
    }
    const record_claims::ObjectiveAdvance outcome =
        record_claims::advance_single_objective(record.completionFlagIndex);
    switch (outcome) {
    case record_claims::ObjectiveAdvance::advanced:
        g_lastGranted.store(definitionIndex, std::memory_order_relaxed);
        return GrantOutcome::progressed;
    case record_claims::ObjectiveAdvance::completed:
        g_lastGranted.store(definitionIndex, std::memory_order_relaxed);
        return GrantOutcome::granted;
    case record_claims::ObjectiveAdvance::alreadyHeld:
        return GrantOutcome::alreadyHeld;
    case record_claims::ObjectiveAdvance::unavailable:
        return GrantOutcome::refused;
    }
    return GrantOutcome::refused;
}

/** Resolves a type-2 collectible through its authored lore-row join. */
GrantOutcome grant_collectible(std::uint16_t collectibleIndex) noexcept {
    namespace collectibles = build_data::collectibles;
    namespace records = build_data::records;
    (void)ensure_records_published();

    collectibles::Definition collectible{};
    if (!build_data::find_collectible_definition(collectibleIndex, collectible)) {
        return GrantOutcome::collectibleNotFound;
    }
    if (collectible.loreRow == collectibles::kUnavailableLoreRow) {
        return GrantOutcome::collectibleNoLore;
    }
    records::Definition record{};
    if (!records::find_by_lore_row(collectible.loreRow, record)) {
        return GrantOutcome::loreRecordNotFound;
    }
    return grant_record(record.definitionIndex);
}

/** @return The record row the last successful grant claimed. */
std::uint16_t last_granted_record() noexcept {
    return g_lastGranted.load(std::memory_order_relaxed);
}

} // namespace sunrise::state::lore
