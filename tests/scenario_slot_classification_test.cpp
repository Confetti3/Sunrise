#include <cstdint>

#include "client/content/scenarios/internal.h"

namespace scenarios = sunrise::client::content::scenarios;
namespace tables = sunrise::middleware::content::packages::tables;

namespace {

scenarios::RosterStorage g_storage{};

void reset_slots() {
    g_storage.slots = {};
    g_storage.slotCount = 0;
    g_storage.slotsOverflowed = false;
    g_storage.descriptorWalkIncomplete = false;
    g_storage.descriptorConflict = false;
}

tables::SlotDescriptor descriptor(std::uint16_t index, std::uint16_t type) {
    tables::SlotDescriptor value{};
    value.slotIndex = index;
    value.slotType = type;
    value.authSchema = tables::kAbsentSchema;
    value.senseSchema = tables::kAbsentSchema;
    return value;
}

} // namespace

int main() {
    g_storage.declaredSlotTypes[0] = 13;
    g_storage.declaredSlotTypes[2] = 17;
    scenarios::record_slot(g_storage, descriptor(2, 17));
    scenarios::record_slot(g_storage, descriptor(0, 13));

    scenarios::layouts::RosterGroup sparse{};
    if (!scenarios::fill_slots(g_storage, 4, sparse) || sparse.slotCount != 2
        || sparse.slotIndices[0] != 0 || sparse.slotIndices[1] != 2
        || sparse.slotTypes[0] != 13 || sparse.slotTypes[1] != 17) {
        return 1;
    }

    reset_slots();
    g_storage.declaredSlotTypes[4] = 17;
    scenarios::record_slot(g_storage, descriptor(4, 17));
    scenarios::layouts::RosterGroup outOfRange{};
    if (scenarios::fill_slots(g_storage, 4, outOfRange)) {
        return 2;
    }

    reset_slots();
    scenarios::layouts::RosterGroup empty{};
    if (scenarios::fill_slots(g_storage, 4, empty)) {
        return 3;
    }
    reset_slots();
    g_storage.declaredSlotTypes[1] = 13;
    scenarios::record_slot(g_storage, descriptor(1, 17));
    scenarios::layouts::RosterGroup wrongType{};
    if (scenarios::fill_slots(g_storage, 4, wrongType)) {
        return 4;
    }
    reset_slots();
    g_storage.declaredSlotTypes[1] = 17;
    scenarios::record_slot(g_storage, descriptor(1, 17));
    g_storage.descriptorWalkIncomplete = true;
    scenarios::layouts::RosterGroup incomplete{};
    if (scenarios::fill_slots(g_storage, 4, incomplete)) {
        return 5;
    }
    reset_slots();
    g_storage.declaredSlotTypes[1] = 17;
    scenarios::record_slot(g_storage, descriptor(1, 17));
    scenarios::record_slot(g_storage, descriptor(1, 13));
    scenarios::layouts::RosterGroup conflicting{};
    if (scenarios::fill_slots(g_storage, 4, conflicting)) {
        return 6;
    }
    reset_slots();
    scenarios::record_slot(g_storage, descriptor(1, 0));
    scenarios::layouts::RosterGroup invalid{};
    if (scenarios::fill_slots(g_storage, 4, invalid)) {
        return 7;
    }
    return 0;
}
