/**
 * Turns the descriptors one placed object declares into the slots activity message 5 publishes.
 * Nothing here reads a package: the walk that follows a descriptor chain owns that, and this owns
 * what the descriptors already say. Keeping the two apart is what lets the classification be
 * checked without an installed content tree.
 */

#include <algorithm>

#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

} // namespace

/** Records one descriptor as a slot of the object being resolved. */
void record_slot(RosterStorage& storage, const tables::SlotDescriptor& descriptor) noexcept {
    if (descriptor.slotType == 0 || descriptor.slotType > layouts::kMaximumSlotType
        || descriptor.slotIndex >= layouts::kRosterSlotCapacity) {
        storage.descriptorWalkIncomplete = true;
        return;
    }
    for (std::size_t slot = 0; slot < storage.slotCount; ++slot) {
        if (storage.slots[slot].index == descriptor.slotIndex) {
            const std::uint8_t flags = slot_flags(descriptor.authSchema, descriptor.senseSchema);
            if (storage.slots[slot].type != descriptor.slotType
                || storage.slots[slot].flags != flags) {
                storage.descriptorConflict = true;
            }
            return;
        }
    }
    if (storage.slotCount == storage.slots.size()) {
        storage.slotsOverflowed = true;
        return;
    }
    storage.slots[storage.slotCount] = {descriptor.slotIndex,
                                        static_cast<std::uint8_t>(descriptor.slotType),
                                        slot_flags(descriptor.authSchema, descriptor.senseSchema)};
    ++storage.slotCount;
}

/** Fills one candidate group from the descriptors the walk found, in slot-index order. */
bool fill_slots(RosterStorage& storage,
                std::size_t declaredSlotCount,
                layouts::RosterGroup& group) noexcept {
    // The object slot array is the complete schema. Its placed handles name the subset this
    // activity actually registers, and their descriptor indices may therefore be sparse.
    if (storage.slotsOverflowed || storage.descriptorWalkIncomplete
        || storage.descriptorConflict || storage.slotCount == 0) {
        return false;
    }
    const auto last = storage.slots.begin() + static_cast<std::ptrdiff_t>(storage.slotCount);
    // The client reads each block independently, but ascending order is what the captured bodies
    // carry and it keeps a body diffable against them.
    std::sort(storage.slots.begin(), last, [](const SlotRecord& first, const SlotRecord& second) {
        return first.index < second.index;
    });
    for (std::size_t slot = 0; slot < storage.slotCount; ++slot) {
        if (storage.slots[slot].index >= declaredSlotCount
            || storage.declaredSlotTypes[storage.slots[slot].index]
                   != storage.slots[slot].type) {
            return false;
        }
        group.slotTypes[slot] = storage.slots[slot].type;
        group.slotFlags[slot] = storage.slots[slot].flags;
        group.slotIndices[slot] = storage.slots[slot].index;
    }
    group.slotCount = static_cast<std::uint16_t>(storage.slotCount);
    return true;
}

} // namespace sunrise::client::content::scenarios
