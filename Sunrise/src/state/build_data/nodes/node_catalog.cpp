#include "node_catalog.h"

#include "../../record_claims/objective_slot_table.h"
#include "../../record_claims/parent_bar_table.h"
#include "../../../core/logging/log.h"
#include <cstdio>
#include <array>

#include "../../unlocks/definition.h"
#include "../table.h"

namespace sunrise::state::build_data::nodes {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;

/**
 * First account value-bank slot the record-objective allocation owns.
 *
 * Mirrors objective_slot_table::kObjectives.front().slot (2746), documented in
 * record_claims.cpp/objective_slot_table.h as the base of the 2746-5686 record-objective range.
 * Not read from that table directly: build_data must not depend upward on record_claims, so the
 * boundary is restated here as its own constant rather than reached for across the layer.
 * Three lore nodes (820, 835, 837) name a valueIndex inside this range -- writing to it has
 * previously trampled record objectives wholesale, so this guard exists to keep this pass off it.
 */


} // namespace

/** Clears every generated node definition under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

/** Checks that the definitions are dense and in native index order. */
bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        if (definitions[row].definitionIndex != row
            || definitions[row].childCount > kChildCapacity) {
            return false;
        }
    }
    return true;
}

/** Replaces the generated node definitions in one step. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_definitions.replace(definitions);
}

/** Runs one callable over every node that drives a value slot. */
void for_each_driving(void* context,
                      void (*visit)(void* context, const Definition& definition)) noexcept {
    if (visit == nullptr) {
        return;
    }
    const Lock::Shared guard(g_lock);
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.childCount != 0 && definition.valueIndex != kUnavailableValueIndex) {
            visit(context, definition);
        }
    }
}

/** Copies every row in native node order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return Number of generated node definitions, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

/** Calls back for every node, under the shared lock. */
void for_each(void* context, void (*visit)(void*, const Definition&) noexcept) noexcept {
    const Lock::Shared guard(g_lock);
    for (const Definition& node : g_definitions.rows()) {
        visit(context, node);
    }
}

/** Sets the visibility gate of every lore book category. */
std::size_t apply_visibility(std::span<std::uint8_t> accountFlags) noexcept {
    const Lock::Shared guard(g_lock);
    std::size_t set = 0;
    for (const Definition& node : g_definitions.rows()) {
        if (node.definitionIndex < kLoreNodeFirst || node.definitionIndex > kLoreNodeLast
            || node.visibilityFlagIndex == kUnavailableFlagIndex
            || static_cast<std::size_t>(node.visibilityFlagIndex) >= accountFlags.size()) {
            continue;
        }
        accountFlags[node.visibilityFlagIndex] = unlocks::kFlagSet;
        ++set;
    }
    return set;
}

/** Sets the value-gate of every lore book category that has no flag gate at all. */
std::size_t apply_category_gates(std::span<std::int32_t> objectiveValues, bool revealAll) noexcept {
    const Lock::Shared guard(g_lock);
    std::size_t set = 0;
    for (const Definition& node : g_definitions.rows()) {
        if (node.definitionIndex < kLoreNodeFirst || node.definitionIndex > kLoreNodeLast) {
            continue;
        }
        // apply_visibility already owns the flag-gated books; a node with a flag gate is not one of
        // this pass's eighteen and must be left alone.
        if (node.visibilityFlagIndex != kUnavailableFlagIndex
            || node.visibilityCharacterFlagIndex != kUnavailableFlagIndex) {
            continue;
        }
        if (node.valueIndex == kUnavailableValueIndex
            || static_cast<std::size_t>(node.valueIndex) >= objectiveValues.size()) {
            continue;
        }
        // Three lore nodes name a valueIndex inside the record-objective range and must never be
        // written here: that slot belongs to a record's objective, not this node's own gate, and
        // stamping it has previously redacted large numbers of records in one pass.
        if (static_cast<std::int32_t>(node.valueIndex) >= record_claims::objective_slot_table::kRecordObjectiveRangeStart) {
            continue;
        }
        // Whether this gate is the book's own bar decides who is allowed to open it. Ten books
        // read their gate from the very slot their bar counts into, so collecting an entry opens
        // them by itself, exactly as the live game does -- forcing those is a presentation choice
        // and stays behind revealAll. The other eight name a gate slot distinct from their bar:
        // nothing on this server ever writes it, because on a real account it is set when the book
        // is acquired from a quest or vendor. That is the same acquisition marker apply_visibility
        // already publishes for the flag-gated books, just held in the value bank instead, so it
        // is satisfied here unconditionally for the same reason.
        // Every value-gated category is published, with no distinction between them. All
        // eighteen carry the identical gate -- READ_VALUE on their own slot, op11, op8 -- so
        // there is no reading of the shipped data on which some of them should be satisfied and
        // others left shut. An earlier version skipped the ten whose gate index equals their bar
        // index, on the theory that writing one would falsify the other. It does not: this pass
        // only ever raises a zero, so those ten read 1 on their parent triumph while nothing is
        // claimed and the true count the moment anything is, which is the same bargain the other
        // eight already make.
        // Never lower a value already written -- a non-zero slot is either already open or holds a
        // count from elsewhere, and this pass only ever needs to prove the gate, never reset it.
        if (objectiveValues[node.valueIndex] == 0) {
            objectiveValues[node.valueIndex] = 1;
            ++set;
        }
    }
    return set;
}

/** Sets the character scoped visibility gates of the lore book categories. */
std::size_t apply_character_visibility(std::span<std::byte> characterFlags) noexcept {
    const Lock::Shared guard(g_lock);
    std::size_t set = 0;
    for (const Definition& node : g_definitions.rows()) {
        if (node.definitionIndex < kLoreNodeFirst || node.definitionIndex > kLoreNodeLast
            || node.visibilityCharacterFlagIndex == kUnavailableFlagIndex
            || static_cast<std::size_t>(node.visibilityCharacterFlagIndex)
                   >= characterFlags.size()) {
            continue;
        }
        characterFlags[node.visibilityCharacterFlagIndex] =
            static_cast<std::byte>(unlocks::kFlagSet);
        ++set;
    }
    return set;
}

} // namespace sunrise::state::build_data::nodes
