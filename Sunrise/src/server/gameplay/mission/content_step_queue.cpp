#include "content_step_queue.h"

#include <Windows.h>

#include <array>
#include <limits>

namespace sunrise::server::gameplay::mission {
namespace {
constexpr std::size_t kCapacity = 32;
struct Slot final { QueuedContentStep value{}; bool occupied{}; };
struct ObservationCursor final {
    std::uint64_t activitySessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t bindingGeneration{};
    std::uint64_t stepId{};
    std::uint32_t generation{};
    bool occupied{};
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Slot, kCapacity> g_slots{};
std::array<ObservationCursor, kCapacity> g_observationCursors{};
std::uint64_t g_nextTicket = 1;

[[nodiscard]] bool expects_observation(ContentStepKind kind) noexcept {
    return kind == ContentStepKind::glimmerSite0ShipSpawn
           || kind == ContentStepKind::glimmerSite1ShipSpawn
           || kind == ContentStepKind::glimmerSite2ShipSpawn;
}
}

bool reserve_content_step(std::uint64_t activitySessionId,
                          std::uint64_t hostGeneration,
                          std::uint64_t bindingGeneration,
                          const ContentStepIntent& intent,
                          ContentStepTicket& ticket) noexcept {
    ticket = {};
    if (activitySessionId == 0 || hostGeneration == 0 || bindingGeneration == 0
        || intent.commandId == 0 || intent.stepId == 0
        || intent.kind == ContentStepKind::count) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (const Slot& slot : g_slots) {
        if (slot.occupied && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration
            && slot.value.intent.commandId == intent.commandId) {
            const bool exact = !slot.value.settled
                               && slot.value.intent.stepId == intent.stepId
                               && slot.value.intent.kind == intent.kind;
            if (exact) ticket = slot.value.ticket;
            ReleaseSRWLockExclusive(&g_lock);
            return exact;
        }
    }
    Slot* free = nullptr;
    for (Slot& slot : g_slots) {
        const bool supersededObservation = slot.occupied && slot.value.settled
                                           && !slot.value.observed
                                           && expects_observation(slot.value.intent.kind)
                                           && slot.value.activitySessionId == activitySessionId
                                           && slot.value.hostGeneration == hostGeneration
                                           && slot.value.bindingGeneration == bindingGeneration
                                           && slot.value.intent.stepId == intent.stepId;
        if (supersededObservation) slot = {};
        if (!slot.occupied && free == nullptr) free = &slot;
    }
    if (free == nullptr || g_nextTicket == (std::numeric_limits<std::uint64_t>::max)()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    free->value = {{g_nextTicket++},
                   intent,
                   activitySessionId,
                   hostGeneration,
                   bindingGeneration,
                   ContentStepStage::publish,
                   false,
                   false,
                   false,
                   false,
                   false,
                   false,
                   0,
                   false,
                   0};
    free->occupied = true;
    ticket = free->value.ticket;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool peek_content_step(std::uint64_t activitySessionId,
                       std::uint64_t hostGeneration,
                       std::uint64_t bindingGeneration,
                       QueuedContentStep& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Slot* selected = nullptr;
    for (const Slot& slot : g_slots) {
        if (slot.occupied && slot.value.producerReady && !slot.value.settled
            && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration
            && (selected == nullptr || slot.value.ticket.value < selected->value.ticket.value)) {
            selected = &slot;
        }
    }
    if (selected != nullptr) output = selected->value;
    ReleaseSRWLockShared(&g_lock);
    return selected != nullptr;
}

bool stage_content_step(std::uint64_t activitySessionId,
                        std::uint64_t hostGeneration,
                        std::uint64_t bindingGeneration,
                        QueuedContentStep& output) noexcept {
    output = {};
    AcquireSRWLockExclusive(&g_lock);
    Slot* selected = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.producerReady && !slot.value.settled
            && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration
            && (selected == nullptr || slot.value.ticket.value < selected->value.ticket.value)) {
            selected = &slot;
        }
    }
    if (selected == nullptr || selected->value.staged
        || (selected->value.cancelPending && !selected->value.published)) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (!selected->value.published) selected->value.staged = true;
    output = selected->value;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool commit_content_step(ContentStepTicket ticket) noexcept {
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.ticket.value == ticket.value
            && !slot.value.published) {
            slot.value.producerReady = true;
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool commit_staged_content_step(ContentStepTicket ticket,
                                QueuedContentStep& output) noexcept {
    output = {};
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.producerReady && slot.value.staged
            && !slot.value.published && slot.value.ticket.value == ticket.value) {
            if (expects_observation(slot.value.intent.kind)) {
                for (const ObservationCursor& cursor : g_observationCursors) {
                    if (cursor.occupied
                        && cursor.activitySessionId == slot.value.activitySessionId
                        && cursor.hostGeneration == slot.value.hostGeneration
                        && cursor.bindingGeneration == slot.value.bindingGeneration
                        && cursor.stepId == slot.value.intent.stepId) {
                        slot.value.baselineGeneration = cursor.generation;
                        slot.value.baselineReady = true;
                        break;
                    }
                }
            }
            slot.value.published = true;
            slot.value.staged = false;
            output = slot.value;
            if (slot.value.cancelPending) slot = {};
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool discard_staged_content_step(ContentStepTicket ticket) noexcept {
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.staged
            && slot.value.ticket.value == ticket.value) {
            if (slot.value.cancelPending) {
                slot = {};
            } else {
                slot.value.staged = false;
            }
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool claim_content_step_observation(std::uint64_t activitySessionId,
                                    std::uint64_t hostGeneration,
                                    std::uint64_t bindingGeneration,
                                    std::uint64_t stepId,
                                    std::uint32_t generation,
                                    ContentStepObservation& observation) noexcept {
    observation = {};
    if (activitySessionId == 0 || hostGeneration == 0 || bindingGeneration == 0
        || stepId == 0 || generation == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    Slot* selected = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration
            && slot.value.intent.stepId == stepId && slot.value.producerReady
            && slot.value.published && !slot.value.cancelPending
            && expects_observation(slot.value.intent.kind) && !slot.value.observed
            && (selected == nullptr
                || slot.value.ticket.value < selected->value.ticket.value)) {
            selected = &slot;
        }
    }
    ObservationCursor* cursor = nullptr;
    ObservationCursor* freeCursor = nullptr;
    for (ObservationCursor& candidate : g_observationCursors) {
        if (candidate.occupied && candidate.activitySessionId == activitySessionId
            && candidate.hostGeneration == hostGeneration
            && candidate.bindingGeneration == bindingGeneration
            && candidate.stepId == stepId) {
            cursor = &candidate;
            break;
        }
        if (!candidate.occupied && freeCursor == nullptr) freeCursor = &candidate;
    }
    if (cursor == nullptr) {
        // Orphan telemetry can fill the bounded cursor table. A published ticket must not stall
        // behind an entry no observation-bearing queue slot can still use. With the cursor and
        // slot tables at equal capacity, reclaiming one unreferenced entry guarantees room for
        // every selected ticket without stealing another live ticket's baseline.
        if (freeCursor == nullptr && selected != nullptr) {
            for (ObservationCursor& candidate : g_observationCursors) {
                bool referenced = false;
                for (const Slot& slot : g_slots) {
                    if (slot.occupied && expects_observation(slot.value.intent.kind)
                        && slot.value.activitySessionId == candidate.activitySessionId
                        && slot.value.hostGeneration == candidate.hostGeneration
                        && slot.value.bindingGeneration == candidate.bindingGeneration
                        && slot.value.intent.stepId == candidate.stepId) {
                        referenced = true;
                        break;
                    }
                }
                if (!referenced) {
                    freeCursor = &candidate;
                    break;
                }
            }
        }
        if (freeCursor != nullptr) {
            *freeCursor = {activitySessionId,
                           hostGeneration,
                           bindingGeneration,
                           stepId,
                           generation,
                           true};
            cursor = freeCursor;
        }
        // A first-use dropship object may not exist until this exact ticket is published. Its
        // first post-publication snapshot establishes only a baseline; it is never a receipt.
        // A later, strictly newer generation is still required to claim the ticket.
        if (selected != nullptr && !selected->value.baselineReady) {
            selected->value.baselineGeneration = generation;
            selected->value.baselineReady = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (selected == nullptr) {
        if (generation > cursor->generation) cursor->generation = generation;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (!selected->value.baselineReady) {
        selected->value.baselineGeneration = generation;
        selected->value.baselineReady = true;
        if (generation > cursor->generation) cursor->generation = generation;
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (generation <= selected->value.baselineGeneration
        || generation <= cursor->generation) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    cursor->generation = generation;
    observation = {selected->value.ticket,
                   selected->value.activitySessionId,
                   selected->value.hostGeneration,
                   selected->value.bindingGeneration,
                   selected->value.intent.stepId,
                   generation};
    if (selected->value.settled) {
        *selected = {};
    } else {
        selected->value.observed = true;
        selected->value.observationGeneration = generation;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool settle_content_step(ContentStepTicket ticket) noexcept {
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.producerReady
            && slot.value.ticket.value == ticket.value && slot.value.published
            && !slot.value.settled) {
            if (slot.value.cancelPending) {
                slot = {};
            } else if (expects_observation(slot.value.intent.kind)
                       && !slot.value.observed) {
                slot.value.settled = true;
            } else {
                slot = {};
            }
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool cancel_content_step(ContentStepTicket ticket) noexcept {
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.ticket.value == ticket.value
            && !slot.value.producerReady && !slot.value.published) {
            slot = {};
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

void cancel_content_steps(std::uint64_t activitySessionId,
                          std::uint64_t hostGeneration,
                          std::uint64_t bindingGeneration) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration) {
            if (slot.value.staged) {
                slot.value.cancelPending = true;
            } else {
                slot = {};
            }
        }
    }
    for (ObservationCursor& cursor : g_observationCursors) {
        if (cursor.occupied && cursor.activitySessionId == activitySessionId
            && cursor.hostGeneration == hostGeneration
            && cursor.bindingGeneration == bindingGeneration) cursor = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void reset_content_steps() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.staged) {
            slot.value.cancelPending = true;
        } else {
            slot = {};
        }
    }
    g_observationCursors = {};
    // Ticket handles never repeat. A BAP-staged pre-reset body stays pinned until its connection
    // commits or discards it, and cannot alias a later reservation.
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::gameplay::mission
