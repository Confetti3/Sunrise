#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/**
 * Records one descriptor as a slot of the object being resolved.
 * @param context Roster storage.
 * @param descriptor Descriptor read from a placed-object blob.
 * @return Always true, because a descriptor this pass cannot use is ordinary.
 */
bool collect_slot(void* context, const tables::SlotDescriptor& descriptor) noexcept {
    record_slot(*static_cast<RosterStorage*>(context), descriptor);
    return true;
}

/** Package state held across one injected descriptor-chain read. */
struct ChainReadContext {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    RosterStorage* storage{};
};

/** Reads one descriptor-chain tag into the roster pass's borrowed blob storage. */
[[nodiscard]] bool read_chain_tag(void* context,
                                  std::uint32_t tag,
                                  std::span<const std::byte>& blob,
                                  std::uint32_t& classId) noexcept {
    auto& chain = *static_cast<ChainReadContext*>(context);
    ++chain.storage->reads;
    if (!reader::read_tag(*chain.source, *chain.scratch, tag, chain.storage->chain, classId)) {
        blob = {};
        return false;
    }
    blob = std::span<const std::byte>{chain.storage->chain};
    return true;
}

/**
 * Follows every branch of one placed handle and records what its descriptor blobs declare.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param handle Tag from a placed object's per-bubble sub-block.
 * @param registryKey Registry key the descriptors must name.
 * @return True only when the bounded chain and complete descriptor scan finished.
 */
[[nodiscard]] bool follow_handle(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 RosterStorage& storage,
                                 std::uint32_t handle,
                                 std::uint32_t registryKey) noexcept {
    ChainReadContext context{&source, &scratch, &storage};
    return tables::walk_slot_descriptor_chain(
        handle, registryKey, &read_chain_tag, &context, &collect_slot, &storage);
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
 * @return True only when every declared placed handle was read and walked completely.
 */
[[nodiscard]] bool collect_descriptors(const reader::Source& source,
                                       reader::Scratch& scratch,
                                       RosterStorage& storage,
                                       std::span<const std::byte> objectBlob,
                                       std::uint32_t registryKey) noexcept {
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
            if (!follow_handle(source, scratch, storage, handle, registryKey)) {
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
    storage.memo[slot].tag = objectTag;
    storage.memo[slot].group = kNotARosterGroup;
    ++storage.reads;
    if (!reader::read_tag(source, scratch, objectTag, storage.object)) {
        return true;
    }

    layouts::RosterGroup candidate{};
    tables::Array declared{};
    if (!tables::object_key(storage.object, candidate.registryKey) || candidate.registryKey == 0
        || !tables::carries_roster_slot(storage.object)
        || !tables::object_slots(storage.object, declared) || declared.count == 0
        || declared.count > layouts::kRosterSlotCapacity) {
        return true;
    }
    storage.slotCount = 0;
    storage.slotsOverflowed = false;
    if (!collect_descriptors(source, scratch, storage, storage.object, candidate.registryKey)
        || !fill_slots(storage, declared.count, candidate)) {
        // A completed walk may prove that some declared slots have no descriptor. A failed walk
        // cannot distinguish that absence from unread content, so it refuses the whole group.
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

} // namespace sunrise::client::content::scenarios
