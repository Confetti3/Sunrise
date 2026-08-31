#include "activity_entity_slot_republish.h"
#include "activity_roster_research.h"

#include <Windows.h>

#include <limits>

#include "../../../../../state/activity/entity_slots/runtime.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../runtime.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

struct EntitySlotRepublishRequest final {
    state::activity::SessionBinding binding{};
    std::uint64_t bindingGeneration{};
    std::uint64_t token{};
};

SRWLOCK g_entitySlotRepublishLock = SRWLOCK_INIT;
EntitySlotRepublishRequest g_entitySlotRepublishRequest{};
EntitySlotRepublishStatus g_entitySlotRepublishStatus{};
std::uint64_t g_entitySlotRepublishNext{1};
std::uint64_t g_lastPublicRejectedToken{};
std::uint64_t g_lastNoPrivateStageToken{};

[[nodiscard]] bool same_generation(const state::activity::SessionBinding& left,
                                   const state::activity::SessionBinding& right) noexcept {
    return left.sessionId != state::activity::kAbsentSessionId
           && left.sessionId == right.sessionId
           && left.createdRevision != state::activity::kInvalidRevision
           && left.createdRevision == right.createdRevision;
}

void reject_stale(std::uint64_t token) noexcept {
    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    if (token != 0 && g_entitySlotRepublishRequest.token == token) {
        g_entitySlotRepublishRequest = {};
        g_entitySlotRepublishStatus.pendingToken = 0;
        g_entitySlotRepublishStatus.pendingBindingGeneration = 0;
        ++g_entitySlotRepublishStatus.staleRejected;
    }
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
}

} // namespace

std::uint64_t request_entity_slot_republish() noexcept {
    ActivitySnapshot snapshot{};
    if (!::sunrise::server::bap::snapshot_private_activity(snapshot)
        || snapshot.bindingGeneration == 0) {
        AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
        ++g_entitySlotRepublishStatus.noPrivateRejected;
        ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
        return 0;
    }
    if (!state::activity::binding_matches(snapshot.binding)) {
        AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
        ++g_entitySlotRepublishStatus.staleRejected;
        ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
        return 0;
    }

    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    const std::uint64_t token = g_entitySlotRepublishNext;
    if (token == 0) {
        ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
        return 0;
    }
    g_entitySlotRepublishNext =
        token == (std::numeric_limits<std::uint64_t>::max)() ? 0 : token + 1;
    g_entitySlotRepublishRequest = {snapshot.binding, snapshot.bindingGeneration, token};
    ++g_entitySlotRepublishStatus.requested;
    ++g_entitySlotRepublishStatus.bound;
    g_entitySlotRepublishStatus.pendingToken = token;
    g_entitySlotRepublishStatus.pendingBindingGeneration = snapshot.bindingGeneration;
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
    return token;
}

EntitySlotRepublishStatus entity_slot_republish_status() noexcept {
    AcquireSRWLockShared(&g_entitySlotRepublishLock);
    const EntitySlotRepublishStatus result = g_entitySlotRepublishStatus;
    ReleaseSRWLockShared(&g_entitySlotRepublishLock);
    return result;
}

namespace entity_slot_republish {

bool select(const Session& session, Selection& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_entitySlotRepublishLock);
    const EntitySlotRepublishRequest request = g_entitySlotRepublishRequest;
    ReleaseSRWLockShared(&g_entitySlotRepublishLock);
    if (request.token == 0) return false;

    if (!session.authenticated || session.activity.role == ActivityClientRole::none) {
        AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
        if (g_lastNoPrivateStageToken != request.token) {
            g_lastNoPrivateStageToken = request.token;
            ++g_entitySlotRepublishStatus.noPrivateRejected;
        }
        ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
        return false;
    }
    if (session.activity.role == ActivityClientRole::publicTarget) {
        AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
        if (g_lastPublicRejectedToken != request.token) {
            g_lastPublicRejectedToken = request.token;
            ++g_entitySlotRepublishStatus.publicRejected;
        }
        ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
        return false;
    }
    if (session.activity.role != ActivityClientRole::privateCurrent) return false;

    // An older still-live private connection must not reject a request bound to a newer one.
    if (session.activity.bindingGeneration < request.bindingGeneration) return false;
    const bool exactConnection =
        session.activity.bindingGeneration == request.bindingGeneration
        && same_generation(session.activity.session, request.binding)
        && same_generation(session.activity.source, request.binding);
    if (!exactConnection) {
        reject_stale(request.token);
        return false;
    }
    ActivitySnapshot expected{};
    expected.binding = request.binding;
    expected.bindingGeneration = request.bindingGeneration;
    if (!::sunrise::server::bap::newest_private_activity_matches_locked(expected)) {
        reject_stale(request.token);
        return false;
    }

    state::activity::entity_slots::LeaseMask held{};
    if (!state::activity::binding_matches(session.activity.session)
        || !state::activity::binding_matches(session.activity.source)
        || !state::activity::entity_slots::held_mask(request.binding, held)) {
        reject_stale(request.token);
        return false;
    }
    output.held = held;
    output.binding = request.binding;
    output.bindingGeneration = request.bindingGeneration;
    output.token = request.token;
    return true;
}

void mark_staged(const Selection& selection) noexcept {
    if (selection.token == 0) return;
    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    ++g_entitySlotRepublishStatus.staged;
    g_entitySlotRepublishStatus.stagedToken = selection.token;
    g_entitySlotRepublishStatus.stagedBindingGeneration = selection.bindingGeneration;
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
}

void stage_publication(Session& session, const Selection& selection) noexcept {
    if (!session.activityRosterStaged.staged || selection.token == 0) return;
    session.activityRosterStaged.entitySlotRepublishBinding = selection.binding;
    session.activityRosterStaged.entitySlotRepublishBindingGeneration =
        selection.bindingGeneration;
    session.activityRosterStaged.entitySlotRepublishToken = selection.token;
    session.activityRosterStaged.hasEntitySlotRepublish = true;
    mark_staged(selection);
}

void commit_staged_publication(Session& session) noexcept {
    if (!session.activityRosterStaged.staged
        || !session.activityRosterStaged.hasEntitySlotRepublish) return;
    Selection delivered{};
    delivered.binding = session.activityRosterStaged.entitySlotRepublishBinding;
    delivered.bindingGeneration =
        session.activityRosterStaged.entitySlotRepublishBindingGeneration;
    delivered.token = session.activityRosterStaged.entitySlotRepublishToken;
    commit(delivered);
    session.activityRosterStaged.hasEntitySlotRepublish = false;
}

void discard_staged_publication(Session& session) noexcept {
    if (!session.activityRosterStaged.staged
        || !session.activityRosterStaged.hasEntitySlotRepublish) return;
    Selection discarded{};
    discarded.binding = session.activityRosterStaged.entitySlotRepublishBinding;
    discarded.bindingGeneration =
        session.activityRosterStaged.entitySlotRepublishBindingGeneration;
    discarded.token = session.activityRosterStaged.entitySlotRepublishToken;
    discard(discarded);
    session.activityRosterStaged.hasEntitySlotRepublish = false;
}

void mark_encode_failed(std::uint64_t token) noexcept {
    if (token == 0) return;
    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    ++g_entitySlotRepublishStatus.encodeFailed;
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
}

void commit(const Selection& selection) noexcept {
    if (selection.token == 0) return;
    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    if (g_entitySlotRepublishStatus.stagedToken == selection.token
        && g_entitySlotRepublishStatus.stagedBindingGeneration
               == selection.bindingGeneration) {
        ++g_entitySlotRepublishStatus.delivered;
        g_entitySlotRepublishStatus.deliveredToken = selection.token;
        if (g_entitySlotRepublishRequest.token == selection.token
            && g_entitySlotRepublishRequest.bindingGeneration == selection.bindingGeneration
            && same_generation(g_entitySlotRepublishRequest.binding, selection.binding)) {
            g_entitySlotRepublishRequest = {};
            g_entitySlotRepublishStatus.pendingToken = 0;
            g_entitySlotRepublishStatus.pendingBindingGeneration = 0;
        }
        g_entitySlotRepublishStatus.stagedToken = 0;
        g_entitySlotRepublishStatus.stagedBindingGeneration = 0;
    }
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
}

void discard(const Selection& selection) noexcept {
    if (selection.token == 0) return;
    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    if (g_entitySlotRepublishStatus.stagedToken == selection.token
        && g_entitySlotRepublishStatus.stagedBindingGeneration
               == selection.bindingGeneration) {
        ++g_entitySlotRepublishStatus.discarded;
        g_entitySlotRepublishStatus.stagedToken = 0;
        g_entitySlotRepublishStatus.stagedBindingGeneration = 0;
    }
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
}

#if defined(SUNRISE_TESTING)
void reset_for_test() noexcept {
    AcquireSRWLockExclusive(&g_entitySlotRepublishLock);
    g_entitySlotRepublishRequest = {};
    g_entitySlotRepublishStatus = {};
    g_entitySlotRepublishNext = 1;
    g_lastPublicRejectedToken = 0;
    g_lastNoPrivateStageToken = 0;
    ReleaseSRWLockExclusive(&g_entitySlotRepublishLock);
}
#endif

} // namespace entity_slot_republish
} // namespace sunrise::server::bap::encrypted::push::activity
