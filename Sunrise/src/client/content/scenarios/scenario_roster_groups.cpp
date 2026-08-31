#include <array>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/** How many hops the chain from a handle to a descriptor blob may take. */
constexpr std::size_t kChainDepthLimit = 8;

/** Build-86657 definitions for the Trostland squad and its relay dummy. */
constexpr std::uint32_t kTrostlandSpawnerDefinition = 0x80C26B0A;
constexpr std::uint32_t kTrostlandDummyDefinition = 0x80C26950;
constexpr std::uint32_t kTrostlandEventObject = 0x80BE91F7;

struct ProbeVisit final {
    RosterStorage* storage{};
    std::uint32_t objectTag{};
    std::uint32_t registryKey{};
};

[[nodiscard]] const char* fixture_name(std::uint32_t tag) noexcept {
    switch (tag) {
    case kTrostlandSpawnerDefinition:
        return "spawner";
    case kTrostlandDummyDefinition:
        return "dummy_definition";
    case 0x80C261E6:
        return "sequence_intro";
    case 0x80C261EC:
        return "sequence_event_active";
    case 0x80C268B0:
        return "site_0_dropship_enter";
    case 0x80C268B5:
        return "site_0_dropship_exit";
    case 0x80C26937:
        return "site_1_dropship_enter";
    case 0x80C2694C:
        return "site_1_dropship_exit";
    case 0x80C268DA:
        return "site_2_dropship_enter";
    case 0x80C268FE:
        return "site_2_dropship_exit";
    case 0x80C26904:
        return "site_3_dropship_enter";
    case 0x80C2690A:
        return "site_3_dropship_exit";
    default:
        return nullptr;
    }
}

void report_probe(const ProbeVisit& visit,
                  const tables::SlotDescriptor& descriptor,
                  const char* fixture) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=spawner_probe stage=descriptor result=found fixture=%s definition=0x%08X "
                      "object=0x%08X group=0x%08X type=%u slot=%u component=0x%08X sense=0x%08X "
                      "auth=0x%08X",
                      fixture,
                      descriptor.definitionTag,
                      visit.objectTag,
                      visit.registryKey,
                      static_cast<unsigned>(descriptor.slotType),
                      static_cast<unsigned>(descriptor.slotIndex),
                      descriptor.componentClass,
                      descriptor.senseSchema,
                      descriptor.authSchema);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void report_probe_chain(const ProbeVisit& visit,
                        const char* reason,
                        std::uint32_t tag,
                        std::uint32_t classId,
                        std::size_t depth) noexcept {
    if (!core::settings::get().server.activation.trostlandSpawnerProbe
        || (visit.objectTag != 0x80C26607U && visit.objectTag != kTrostlandEventObject)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=spawner_probe stage=descriptor_chain result=fail "
                                      "reason=%s object=0x%08X group=0x%08X tag=0x%08X "
                                      "class=0x%08X depth=%zu",
                                      reason,
                                      visit.objectTag,
                                      visit.registryKey,
                                      tag,
                                      classId,
                                      depth);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Records one descriptor as a slot of the object being resolved.
 * @param context Roster storage and the owning object identity.
 * @param descriptor Descriptor read from a placed-object blob.
 * @return Always true, because a descriptor this pass cannot use is ordinary.
 */
bool collect_slot(void* context, const tables::SlotDescriptor& descriptor) noexcept {
    auto& visit = *static_cast<ProbeVisit*>(context);
    if (core::settings::get().server.activation.trostlandSpawnerProbe) {
        if (const char* fixture = fixture_name(descriptor.definitionTag); fixture != nullptr) {
            ++visit.storage->probeDescriptorCount;
            report_probe(visit, descriptor, fixture);
        } else if (visit.objectTag == 0x80C26607U
                   || visit.objectTag == kTrostlandEventObject) {
            ++visit.storage->probeDescriptorCount;
            report_probe(visit, descriptor, "event_slot");
        }
    }
    record_slot(*visit.storage, descriptor);
    return true;
}

/**
 * Follows one placed handle to its descriptor blob and records what it declares.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param visit Working storage and owning object identity for this pass.
 * @param handle Tag from a placed object's per-bubble sub-block.
 */
[[nodiscard]] bool follow_handle(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 ProbeVisit& visit,
                                 std::uint32_t handle) noexcept {
    RosterStorage& storage = *visit.storage;
    std::uint32_t tag = handle;
    for (std::size_t depth = 0; depth < kChainDepthLimit; ++depth) {
        std::uint32_t classId = 0;
        ++storage.reads;
        if (!reader::read_tag(source, scratch, tag, storage.chain, classId)) {
            report_probe_chain(visit, "read", tag, classId, depth);
            return false;
        }
        if (classId == tables::kPlacedObjectClass) {
            return tables::visit_slot_descriptors(
                storage.chain, tag, visit.registryKey, &collect_slot, &visit);
        }
        std::uint32_t next = 0;
        if (!tables::next_descriptor_tag(storage.chain, classId, next)) {
            report_probe_chain(visit, "class", tag, classId, depth);
            // Placement arrays also carry terminal, non-component definitions. They contribute no
            // type-5 slot and are complete without a descriptor chain.
            return true;
        }
        tag = next;
    }
    report_probe_chain(visit, "depth", tag, 0, kChainDepthLimit);
    return false;
}

/**
 * Collects every descriptor one group object declares, over all of its per-bubble sub-blocks.
 * Every leaf is followed: one leaf is one slot, so stopping early would drop slots rather than
 * merely leave a slot type unresolved.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage receiving the descriptors.
 * @param objectBlob Whole placed-object bytes.
 * @param registryKey Registry key the descriptors must name.
 */
[[nodiscard]] bool collect_descriptors(const reader::Source& source,
                                       reader::Scratch& scratch,
                                       RosterStorage& storage,
                                       std::span<const std::byte> objectBlob,
                                       std::uint32_t objectTag,
                                       std::uint32_t registryKey) noexcept {
    ProbeVisit visit{&storage, objectTag, registryKey};
    tables::Array bubbles{};
    if (!tables::object_bubbles(objectBlob, bubbles)) {
        return false;
    }
    for (std::uint64_t index = 0; index < bubbles.count; ++index) {
        tables::ObjectBubble bubble{};
        if (!tables::object_bubble_at(objectBlob, bubbles, index, bubble)) {
            return false;
        }
        for (std::uint64_t slot = 0; slot < bubble.handleCount; ++slot) {
            std::uint32_t handle = 0;
            if (!tables::object_placed_handle_at(objectBlob, bubble, slot, handle)) {
                return false;
            }
            if (!follow_handle(source, scratch, visit, handle)) {
                return false;
            }
        }
    }
    return true;
}

/** @param storage Working storage. @param tag Object tag. @return Its memo slot, or capacity. */
[[nodiscard]] std::size_t memo_slot(const RosterStorage& storage, std::uint32_t tag) noexcept {
    std::size_t probe = tag % kObjectMemoCapacity;
    for (std::size_t step = 0; step < kObjectMemoCapacity; ++step) {
        if (storage.memo[probe].tag == 0 || storage.memo[probe].tag == tag) {
            return probe;
        }
        probe = (probe + 1) % kObjectMemoCapacity;
    }
    return kObjectMemoCapacity;
}

} // namespace

/**
 * Finds the roster group of one placed object, reading it only the first time it is seen.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param objectTag Tag from an object registry.
 * @param group Receives the roster group index, or the not-a-group sentinel.
 * @return True when the object was read or was already known.
 */
bool resolve_object(const reader::Source& source,
                    reader::Scratch& scratch,
                    RosterStorage& storage,
                    std::uint32_t objectTag,
                    std::uint16_t& group) noexcept {
    group = kNotARosterGroup;
    const std::size_t slot = memo_slot(storage, objectTag);
    if (slot == kObjectMemoCapacity) {
        return false;
    }
    if (storage.memo[slot].tag == objectTag) {
        group = storage.memo[slot].group;
        return true;
    }
    ++storage.reads;
    if (!reader::read_tag(source, scratch, objectTag, storage.object)) {
        // Package availability changes during startup. Do not memoize a transient read failure.
        return true;
    }
    storage.memo[slot].tag = objectTag;
    storage.memo[slot].group = kNotARosterGroup;

    layouts::RosterGroup candidate{};
    tables::Array declared{};
    const bool measuredEncounter = objectTag == kTrostlandEventObject
                                   && (core::settings::get().server.activation.trostlandSpawnerProbe
                                       || core::settings::get().server.activation.missionScriptHost);
    if (!tables::object_key(storage.object, candidate.registryKey) || candidate.registryKey == 0) {
        return true;
    }
    if ((!tables::carries_roster_slot(storage.object) && !measuredEncounter)
        || !tables::object_slots(storage.object, declared) || declared.count == 0
        || declared.count > layouts::kRosterSlotCapacity) {
        return true;
    }
    storage.slotCount = 0;
    storage.slotsOverflowed = false;
    storage.descriptorWalkIncomplete = false;
    storage.descriptorConflict = false;
    for (std::uint64_t index = 0; index < declared.count; ++index) {
        tables::Slot declaredSlot{};
        if (!tables::object_slot_at(storage.object, declared, index, declaredSlot)) {
            storage.descriptorWalkIncomplete = true;
            break;
        }
        if (declaredSlot.type == 0 || declaredSlot.type > layouts::kMaximumSlotType) {
            storage.descriptorWalkIncomplete = true;
            break;
        }
        storage.declaredSlotTypes[static_cast<std::size_t>(index)] = declaredSlot.type;
    }
    if (!storage.descriptorWalkIncomplete
        && !collect_descriptors(
            source, scratch, storage, storage.object, objectTag, candidate.registryKey)) {
        storage.descriptorWalkIncomplete = true;
    }
    if (!fill_slots(storage, declared.count, candidate)) {
        // A group with no valid placed descriptors cannot produce a sensor/auth body.
        // An incomplete package read is transient, so leave this object eligible for a later
        // destination or the explicit research retry in the same extraction pass.
        if (storage.descriptorWalkIncomplete) {
            storage.memo[slot] = {};
        }
        ++storage.unresolvedGroups;
        return true;
    }
    candidate.objectTag = objectTag;
    // One key may carry different layouts in different activities, so only exact layouts reuse.
    for (std::size_t index = 0; index < storage.groupCount; ++index) {
        if (same_group_layout(storage.groups[index], candidate)) {
            storage.memo[slot].group = static_cast<std::uint16_t>(index);
            group = storage.memo[slot].group;
            return true;
        }
    }
    if (storage.groupCount == layouts::kRosterGroupCapacity) {
        return false;
    }
    storage.groups[storage.groupCount] = candidate;
    storage.memo[slot].group = static_cast<std::uint16_t>(storage.groupCount);
    group = storage.memo[slot].group;
    ++storage.groupCount;
    return true;
}

/** Reads the Trostland event object even when a cache hit bypasses the normal roster walk. */
bool probe_trostland_roster(const reader::Source& source, reader::Scratch& scratch) noexcept {
    if (!core::settings::get().server.activation.trostlandSpawnerProbe) {
        return true;
    }
    static RosterStorage storage{};
    static bool complete = false;
    if (complete) {
        return true;
    }
    std::uint16_t group = kNotARosterGroup;
    if (!resolve_object(source, scratch, storage, kTrostlandEventObject, group)
        || group == kNotARosterGroup) {
        return false;
    }
    const layouts::RosterGroup& roster = storage.groups[group];
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=spawner_probe stage=event_roster result=found "
                                      "object=0x%08X group=0x%08X slots=%u descriptors=%zu",
                                      roster.objectTag,
                                      roster.registryKey,
                                      static_cast<unsigned>(roster.slotCount),
                                      storage.probeDescriptorCount);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    complete = true;
    return true;
}

} // namespace sunrise::client::content::scenarios
