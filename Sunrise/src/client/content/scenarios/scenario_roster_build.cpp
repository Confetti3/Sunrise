#include <array>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/** The three registry descriptors, walked in this order to match the reference walk. */
constexpr std::array<std::size_t, 3> kRegistryDescriptors = {
    tables::kRegistryFirstDescriptor,
    tables::kRegistrySecondDescriptor,
    tables::kRegistryThirdDescriptor,
};

/** Destinations between progress lines. */
constexpr std::size_t kRosterProgressInterval = 64;
constexpr std::string_view kTrostlandDestination{"edz_freeroam"};
constexpr std::uint32_t kTrostlandEncounterGroup = 0x1F6C5054;
constexpr std::uint32_t kTrostlandEncounterObject = 0x80BE91F7;
constexpr std::uint32_t kTrostlandEncounterSliceSet = 408;
constexpr std::uint64_t kTrostlandEncounterSliceBit = std::uint64_t{1} << 51U;

void report_trostland_group(const layouts::Definition& row,
                            const Walk& walk,
                            const RosterStorage& storage) noexcept {
    if (!core::settings::get().server.activation.trostlandSpawnerProbe) {
        return;
    }
    for (std::size_t index = 0; index < walk.candidateCount; ++index) {
        const Candidate& candidate = walk.candidates[index];
        if (candidate.key != kTrostlandEncounterGroup) {
            continue;
        }
        const layouts::RosterGroup& group = storage.groups[candidate.group];
        std::uint64_t mask = 0;
        for (std::size_t key = 0; key < walk.intersection.keyCount; ++key) {
            if (walk.intersection.keys[key] == candidate.key) {
                mask = walk.intersection.masks[key];
                break;
            }
        }
        std::array<char, core::log::kLineCapacity> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=spawner_probe stage=encounter_group result=found destination=%.*s "
                          "group=0x%08X object=0x%08X slots=%u mask=0x%016llX observed=0x%016llX "
                          "top_level=%u bubble_groups=%u",
                          static_cast<int>(row.nameLength),
                          row.name.data(),
                          group.registryKey,
                          group.objectTag,
                          static_cast<unsigned>(group.slotCount),
                          static_cast<unsigned long long>(mask),
                          static_cast<unsigned long long>(walk.intersection.observedSets),
                          static_cast<unsigned>(row.rosterGroupCount),
                          static_cast<unsigned>(row.bubbleGroupCount));
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    if (std::string_view(row.name.data(), row.nameLength) == kTrostlandDestination) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=spawner_probe stage=encounter_group result=missing "
                         "destination=edz_freeroam group=0x1F6C5054");
    }
}

/**
 * Reports how far the walk has reached.
 * @param walked Destinations walked so far.
 * @param total Destinations to walk.
 * @param groups Roster groups found so far.
 */
void report_progress(std::size_t walked, std::size_t total, std::size_t groups) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=roster walked=%zu of=%zu groups=%zu",
                                      walked,
                                      total,
                                      groups);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** The two slot types a usable roster must carry between all of its groups. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
constexpr std::uint8_t kSlotTypeLifetime = 17;

/**
 * Records one candidate group, or updates the one already recorded for its key.
 * @param walk Accumulator for one destination.
 * @param storage Working storage holding the group table.
 * @param group Roster group index.
 * @param primary True when the object was reached through the destination's own registry array.
 */
void note_candidate(Walk& walk,
                    const RosterStorage& storage,
                    std::uint16_t group,
                    bool primary) noexcept {
    for (std::size_t index = 0; index < walk.candidateCount; ++index) {
        if (walk.candidates[index].group == group) {
            walk.candidates[index].primaryRegistry =
                walk.candidates[index].primaryRegistry || primary;
            return;
        }
    }
    if (walk.candidateCount == walk.candidates.size()) {
        return;
    }
    const layouts::RosterGroup& row = storage.groups[group];
    Candidate& candidate = walk.candidates[walk.candidateCount++];
    candidate.group = group;
    candidate.key = row.registryKey;
    candidate.primaryRegistry = primary;
    for (std::size_t slot = 0; slot < row.slotCount; ++slot) {
        candidate.bindsPlayer =
            candidate.bindsPlayer || row.slotTypes[slot] == kSlotTypeParticipation;
        candidate.reportsLifetime =
            candidate.reportsLifetime || row.slotTypes[slot] == kSlotTypeLifetime;
    }
}

/**
 * Walks one slice-set state to every placed object its registry names.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param walk Accumulator for one destination.
 * @param sliceSetIndex Slice-set index the entry reported.
 * @return True when the registry walked without running out of fixed storage.
 */
[[nodiscard]] bool walk_registry(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 RosterStorage& storage,
                                 Walk& walk,
                                 std::uint32_t sliceSetIndex) noexcept {
    for (std::size_t descriptor = 0; descriptor < kRegistryDescriptors.size(); ++descriptor) {
        tables::Array objects{};
        if (!tables::registry_objects(
                storage.registry, kRegistryDescriptors[descriptor], objects)) {
            // A registry declaring only some of its three arrays is ordinary.
            continue;
        }
        for (std::uint64_t index = 0; index < objects.count; ++index) {
            std::uint32_t objectTag = 0;
            if (!tables::registry_object_at(storage.registry, objects, index, objectTag)) {
                return false;
            }
            std::uint16_t group = kNotARosterGroup;
            if (!resolve_object(source, scratch, storage, objectTag, group)) {
                return false;
            }
            if (group == kNotARosterGroup) {
                continue;
            }
            note_candidate(walk, storage, group, descriptor == 0);
            if (!tables::observe_roster_key(
                    walk.intersection, sliceSetIndex, storage.groups[group].registryKey)) {
                return true;
            }
        }
    }
    return true;
}

/**
 * Walks one destination's scenario to every slice-set state it declares.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param walk Accumulator for this destination.
 * @return True when the scenario read and fixed storage held.
 */
[[nodiscard]] bool walk_destination(const reader::Source& source,
                                    reader::Scratch& scratch,
                                    RosterStorage& storage,
                                    Walk& walk) noexcept {
    tables::Array bubbles{};
    if (!tables::scenario_bubbles(storage.scenario, bubbles)) {
        return false;
    }
    for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
        tables::Bubble bubble{};
        if (!tables::bubble_at(storage.scenario, bubbles, bubbleIndex, bubble)) {
            return false;
        }
        for (std::uint64_t stateIndex = 0; stateIndex < bubble.stateCount; ++stateIndex) {
            tables::SliceState state{};
            if (!tables::slice_state_at(storage.scenario, bubble, stateIndex, state)) {
                return false;
            }
            tables::SliceEntry entry{};
            ++storage.reads;
            if (!reader::read_tag(source, scratch, state.entryTag, storage.entry)
                || !tables::slice_entry(storage.entry, entry)) {
                // The destination still transitions into that slice set, and no key can be proved
                // present in it, so nothing of this destination stays safe.
                tables::observe_unresolved_slice_set(walk.intersection);
                continue;
            }
            const std::uint32_t sliceSetIndex = entry.index * tables::kSliceSetIndexFactor;
            if (!tables::observe_slice_set(walk.intersection, sliceSetIndex)) {
                return true;
            }
            ++storage.reads;
            if (!reader::read_tag(source, scratch, entry.registryTag, storage.registry)) {
                tables::observe_unresolved_slice_set(walk.intersection);
                continue;
            }
            if (!walk_registry(source, scratch, storage, walk, sliceSetIndex)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/**
 * Builds the roster half of the domain for every destination row.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param rows Destination rows whose tag is already set, updated in place.
 * @return True when every row was walked.
 */
bool build_rosters(const reader::Source& source,
                   reader::Scratch& scratch,
                   RosterStorage& storage,
                   std::span<layouts::Definition> rows) noexcept {
    storage.reads = 0;
    while (storage.cursor < rows.size() && storage.reads < kRosterReadBudget) {
        // The walk is thousands of tag reads, so it reports progress rather than going quiet.
        if (storage.cursor % kRosterProgressInterval == 0) {
            report_progress(storage.cursor, rows.size(), storage.groupCount);
        }
        layouts::Definition& row = rows[storage.cursor];
        ++storage.cursor;
        row.rosterGroupCount = 0;
        row.rosterGroups = {};
        row.bubbleGroupCount = 0;
        ++storage.reads;
        if (!reader::read_tag(source, scratch, row.tag, storage.scenario)) {
            continue;
        }
        Walk walk{};
        if (!walk_destination(source, scratch, storage, walk)) {
            continue;
        }
        if ((core::settings::get().server.activation.trostlandSpawnerProbe
             || core::settings::get().server.activation.missionScriptHost)
            && std::string_view(row.name.data(), row.nameLength) == kTrostlandDestination) {
            std::uint16_t encounter = kNotARosterGroup;
            if (!resolve_object(
                    source, scratch, storage, kTrostlandEncounterObject, encounter)) {
                continue;
            }
            if (encounter != kNotARosterGroup
                && storage.groups[encounter].registryKey == kTrostlandEncounterGroup
                && (walk.intersection.observedSets & kTrostlandEncounterSliceBit) != 0) {
                note_candidate(walk, storage, encounter, false);
                (void)tables::observe_roster_key(walk.intersection,
                                                 kTrostlandEncounterSliceSet,
                                                 storage.groups[encounter].registryKey);
            }
        }
        publish_groups(walk, row);
        report_trostland_group(row, walk, storage);
    }
    return storage.cursor >= rows.size();
}

} // namespace sunrise::client::content::scenarios
