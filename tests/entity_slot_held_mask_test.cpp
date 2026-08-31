#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <cstddef>

#include "state/activity/entity_slots/runtime.h"
#include "state/runtime/storage/internal.h"

namespace slots = sunrise::state::activity::entity_slots;
namespace activity = sunrise::state::activity;

namespace sunrise::state::runtime::storage {
State g_state{};
SRWLOCK g_stateLock = SRWLOCK_INIT;
}

void assert_zero(const slots::LeaseMask& mask) {
    assert(std::all_of(mask.begin(), mask.end(), [](std::byte value) {
        return value == std::byte{};
    }));
}

int main() {
    auto& state = sunrise::state::runtime::storage::g_state.activity;
    auto& record = state.sessions[0];
    record.occupied = true;
    record.joined = true;
    record.sessionId = 0x1234;
    record.createdRevision = 17;
    record.recordRevision = 19;
    record.joinedRevision = 19;
    record.memberKey = 0x5566;
    auto& destination = record.destination;
    destination.packageName[0] = 'a';
    destination.packageName[1] = 'b';
    destination.packageNameLength = 2;
    destination.reason = 3;
    destination.previousActivityIndex = 4;
    destination.activityIndex = 7;
    destination.elementIndex = 3;
    destination.arrivalBubbleHash = 0x11112222;
    destination.spawnSetHash = 0x33334444;
    destination.hasElementIndex = true;
    destination.hasArrivalBubbleHash = true;
    destination.hasSpawnSetHash = true;
    destination.arrivalBubbleOverride = 5;
    destination.hasArrivalBubbleOverride = true;
    destination.sliceSetOverride = 6;
    destination.hasSliceSetOverride = true;
    destination.spawnSetOverride = 0x55556666;
    destination.hasSpawnSetOverride = true;
    destination.descriptorBits[0] = std::byte{0x80};
    destination.descriptorBits[17] = std::byte{0x04};
    destination.descriptorBitLength = 140;
    destination.descriptorNameBit = 12;
    destination.hasDescriptorName = true;
    record.heldEntitySlots[0] = std::byte{0x81};
    record.heldEntitySlots[500] = std::byte{0x20};
    record.heldEntitySlots[1023] = std::byte{0x02};
    record.serverEntitySlots[1023] = std::byte{0xF0};
    const auto heldBefore = record.heldEntitySlots;
    const auto serverBefore = record.serverEntitySlots;
    const auto memberBefore = record.memberKey;
    const auto recordRevisionBefore = record.recordRevision;
    const auto stateRevisionBefore = state.stateRevision;

    activity::SessionBinding binding{};
    binding.destination = record.destination;
    binding.sessionId = record.sessionId;
    binding.createdRevision = record.createdRevision;
    slots::LeaseMask selected{};
    assert(slots::held_mask(binding, selected));
    assert(selected == heldBefore);
    assert(selected[1023] == std::byte{0x02}); // server mask was not merged
    assert(record.heldEntitySlots == heldBefore && record.serverEntitySlots == serverBefore
           && record.memberKey == memberBefore
           && record.recordRevision == recordRevisionBefore
           && state.stateRevision == stateRevisionBefore);

    const auto mismatch = [&](auto mutate) {
        activity::SessionBinding changed = binding;
        mutate(changed);
        selected.fill(std::byte{0xFF});
        assert(!slots::held_mask(changed, selected));
        assert_zero(selected);
        assert(record.heldEntitySlots == heldBefore && record.serverEntitySlots == serverBefore
               && record.memberKey == memberBefore
               && record.recordRevision == recordRevisionBefore
               && state.stateRevision == stateRevisionBefore);
    };
    mismatch([](auto& value) { ++value.sessionId; });
    // Same-sessionId recreation must fail even when every destination field is equal.
    mismatch([](auto& value) { ++value.createdRevision; });
    mismatch([](auto& value) { value.destination.packageName[0] = 'z'; });
    mismatch([](auto& value) { ++value.destination.packageNameLength; });
    mismatch([](auto& value) { ++value.destination.reason; });
    mismatch([](auto& value) { ++value.destination.previousActivityIndex; });
    mismatch([](auto& value) { ++value.destination.activityIndex; });
    mismatch([](auto& value) { ++value.destination.elementIndex; });
    mismatch([](auto& value) { ++value.destination.arrivalBubbleHash; });
    mismatch([](auto& value) { ++value.destination.spawnSetHash; });
    mismatch([](auto& value) { value.destination.hasElementIndex = false; });
    mismatch([](auto& value) { value.destination.hasArrivalBubbleHash = false; });
    mismatch([](auto& value) { value.destination.hasSpawnSetHash = false; });
    mismatch([](auto& value) { ++value.destination.arrivalBubbleOverride; });
    mismatch([](auto& value) { value.destination.hasArrivalBubbleOverride = false; });
    mismatch([](auto& value) { ++value.destination.sliceSetOverride; });
    mismatch([](auto& value) { value.destination.hasSliceSetOverride = false; });
    mismatch([](auto& value) { ++value.destination.spawnSetOverride; });
    mismatch([](auto& value) { value.destination.hasSpawnSetOverride = false; });
    mismatch([](auto& value) { value.destination.descriptorBits[17] = std::byte{0x08}; });
    mismatch([](auto& value) { ++value.destination.descriptorBitLength; });
    mismatch([](auto& value) { ++value.destination.descriptorNameBit; });
    mismatch([](auto& value) { value.destination.hasDescriptorName = false; });

    record.heldEntitySlots.fill(std::byte{});
    assert(slots::held_mask(binding, selected));
    assert_zero(selected);
    record.heldEntitySlots.fill(std::byte{0xFF});
    assert(slots::held_mask(binding, selected));
    assert(selected == record.heldEntitySlots);
    assert(record.serverEntitySlots == serverBefore);
    return 0;
}
