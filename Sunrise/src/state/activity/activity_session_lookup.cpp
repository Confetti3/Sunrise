#include <Windows.h>

#include <bit>

#include "../runtime/storage/internal.h"
#include "runtime.h"

namespace sunrise::state::activity {

/** Tests whether a nonzero activity-session id is still in the bounded table. */
bool contains(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool found = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Tests whether a committed activity-session id has finished a join. */
bool is_joined(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool joined = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            joined = record.joined && record.joinedRevision != kInvalidRevision;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return joined;
}

/** Copies the newest occupied record without exposing State-owned storage. */
bool latest_snapshot(RuntimeSnapshot& output) noexcept {
    output = {};
    output.reportedRegion = membership::kAbsentRegionIndex;

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const SessionRecord* newest = nullptr;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied
            && (newest == nullptr || record.createdRevision > newest->createdRevision)) {
            newest = &record;
        }
    }

    if (newest != nullptr) {
        output.destination = newest->destination;
        output.sessionId = newest->sessionId;
        output.joined = newest->joined && newest->joinedRevision != kInvalidRevision;
        output.reportedRegion = newest->membership.region.index;
        for (const std::byte value : newest->heldEntitySlots) {
            output.heldEntitySlots += static_cast<std::uint32_t>(
                std::popcount(std::to_integer<unsigned int>(value)));
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return newest != nullptr;
}

} // namespace sunrise::state::activity
