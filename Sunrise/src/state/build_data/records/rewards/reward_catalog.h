#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::records::rewards {

/** Clears every generated record-reward row. */
void clear() noexcept;

/**
 * Checks that the rows fit fixed storage. Zero rows is valid: it is what an absent or empty
 * shipped table loads as, and is a normal deployment with no generated rewards, not a malformed
 * one.
 * @param rows Candidate rows.
 * @return True when the rows fit storage.
 */
[[nodiscard]] bool valid(std::span<const RewardRow> rows) noexcept;

/**
 * Replaces the generated record-reward table in one step.
 * @param rows Complete replacement rows, in any order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const RewardRow> rows) noexcept;

/**
 * Visits every reward row naming one record, holding the catalog lock for the whole walk.
 * Resolving an item hash against this build's item table is the caller's concern, not the
 * catalog's: this domain only owns the authored rows extracted from the manifest join.
 * @param recordHash records::Definition::definitionHash to match.
 * @param visitor Called once per matching row, in table order. Returning false ends the walk early.
 * @param context Opaque pointer handed back to the visitor.
 */
void visit_for_record(std::uint32_t recordHash, RowVisitor visitor, void* context) noexcept;

/** @return Number of generated reward rows, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::records::rewards
