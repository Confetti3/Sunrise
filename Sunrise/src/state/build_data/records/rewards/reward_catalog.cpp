#include "reward_catalog.h"

#include "../../table.h"

namespace sunrise::state::build_data::records::rewards {
namespace {

Lock g_lock;
Table<RewardRow, kRewardCapacity> g_rows;

} // namespace

/** Clears every generated record-reward row under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_rows.clear();
}

/** Checks that the rows fit fixed storage. Zero rows is valid. */
bool valid(std::span<const RewardRow> rows) noexcept {
    return rows.size() <= kRewardCapacity;
}

/** Replaces the generated record-reward table in one step. */
bool replace(std::span<const RewardRow> rows) noexcept {
    if (!valid(rows)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_rows.replace(rows);
}

/** Visits every reward row naming one record, holding the catalog lock for the whole walk. */
void visit_for_record(std::uint32_t recordHash, RowVisitor visitor, void* context) noexcept {
    const Lock::Shared guard(g_lock);
    for (const RewardRow& row : g_rows.rows()) {
        if (row.recordHash != recordHash) {
            continue;
        }
        if (!visitor(context, row)) {
            return;
        }
    }
}

/** @return Number of generated reward rows, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_rows.count();
}

} // namespace sunrise::state::build_data::records::rewards
