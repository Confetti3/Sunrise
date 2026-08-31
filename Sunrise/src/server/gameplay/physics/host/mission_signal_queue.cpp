#include "mission_signal_queue.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>

namespace sunrise::server::gameplay::physics::host::mission_signal_queue {
namespace {

constexpr std::size_t kSignalCapacity = 16;
constexpr std::size_t kScopeCapacity = 4;
constexpr std::uint64_t kMaxSequence =
    (std::numeric_limits<std::uint64_t>::max)() >> 2U;

struct Scope final {
    std::uint64_t activitySessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t bindingGeneration{};
    bool occupied{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Request, kSignalCapacity> g_requests{};
std::array<Scope, kScopeCapacity> g_scopes{};
std::atomic_uint64_t g_nextSequence{1};

[[nodiscard]] std::uint64_t next_sequence() noexcept {
    std::uint64_t current = g_nextSequence.load(std::memory_order_acquire);
    while (current != 0 && current <= kMaxSequence) {
        if (g_nextSequence.compare_exchange_weak(current,
                                                 current + 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return current;
        }
    }
    return 0;
}

[[nodiscard]] bool scope_registered(std::uint64_t activitySessionId,
                                    std::uint64_t hostGeneration,
                                    std::uint64_t bindingGeneration) noexcept {
    for (const Scope& scope : g_scopes) {
        if (scope.occupied && scope.activitySessionId == activitySessionId
            && scope.hostGeneration == hostGeneration
            && scope.bindingGeneration == bindingGeneration) return true;
    }
    return false;
}

} // namespace

bool register_scope(std::uint64_t activitySessionId,
                    std::uint64_t hostGeneration,
                    std::uint64_t bindingGeneration) noexcept {
    if (activitySessionId == 0 || hostGeneration == 0 || bindingGeneration == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    Scope* free = nullptr;
    for (Scope& scope : g_scopes) {
        if (scope.occupied && scope.activitySessionId == activitySessionId
            && scope.hostGeneration == hostGeneration
            && scope.bindingGeneration == bindingGeneration) {
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
        if (!scope.occupied && free == nullptr) free = &scope;
    }
    if (free != nullptr) {
        *free = {activitySessionId, hostGeneration, bindingGeneration, true};
    }
    ReleaseSRWLockExclusive(&g_lock);
    return free != nullptr;
}

void unregister_scope(std::uint64_t activitySessionId,
                      std::uint64_t hostGeneration,
                      std::uint64_t bindingGeneration) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (Scope& scope : g_scopes) {
        if (scope.occupied && scope.activitySessionId == activitySessionId
            && scope.hostGeneration == hostGeneration
            && scope.bindingGeneration == bindingGeneration) scope = {};
    }
    for (Request& request : g_requests) {
        if (request.occupied && request.scoped
            && request.activitySessionId == activitySessionId
            && request.hostGeneration == hostGeneration
            && request.bindingGeneration == bindingGeneration) request = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::uint64_t reserve(std::uint64_t activitySessionId,
                      std::uint64_t hostGeneration,
                      std::uint64_t bindingGeneration,
                      std::uint64_t signalId,
                      bool scoped,
                      bool ready) noexcept {
    if (signalId == 0
        || (scoped && (activitySessionId == 0 || hostGeneration == 0
                       || bindingGeneration == 0))) return 0;
    const std::uint64_t sequence = next_sequence();
    if (sequence == 0) return 0;
    AcquireSRWLockExclusive(&g_lock);
    if (scoped && !scope_registered(activitySessionId, hostGeneration, bindingGeneration)) {
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }
    Request* free = nullptr;
    for (Request& request : g_requests) {
        if (!request.occupied) {
            free = &request;
            break;
        }
    }
    if (free != nullptr) {
        *free = {signalId,
                 sequence,
                 activitySessionId,
                 hostGeneration,
                 bindingGeneration,
                 scoped,
                 ready,
                 true};
    }
    ReleaseSRWLockExclusive(&g_lock);
    return free == nullptr ? 0 : sequence;
}

bool commit_if(std::uint64_t sequence,
               CommitCondition condition,
               void* context) noexcept {
    if (sequence == 0 || condition == nullptr) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Request& request : g_requests) {
        if (request.occupied && !request.ready && request.sequence == sequence) {
            if (!condition(context)) {
                ReleaseSRWLockExclusive(&g_lock);
                return false;
            }
            request.ready = true;
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool commit(std::uint64_t sequence) noexcept {
    const auto accept = [](void*) noexcept { return true; };
    return commit_if(sequence, accept, nullptr);
}

bool peek(std::uint64_t activitySessionId,
          std::uint64_t hostGeneration,
          std::uint64_t bindingGeneration,
          Request& request) noexcept {
    request = {};
    AcquireSRWLockShared(&g_lock);
    const Request* selected = nullptr;
    for (const Request& candidate : g_requests) {
        const bool matches = !candidate.scoped
                             || (candidate.activitySessionId == activitySessionId
                                 && candidate.hostGeneration == hostGeneration
                                 && candidate.bindingGeneration == bindingGeneration);
        if (candidate.occupied && candidate.ready && matches
            && (selected == nullptr || candidate.sequence < selected->sequence)) {
            selected = &candidate;
        }
    }
    if (selected != nullptr) request = *selected;
    ReleaseSRWLockShared(&g_lock);
    return selected != nullptr;
}

bool consume(std::uint64_t sequence) noexcept {
    if (sequence == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Request& request : g_requests) {
        if (request.occupied && request.sequence == sequence) {
            request = {};
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_requests = {};
    g_scopes = {};
    ReleaseSRWLockExclusive(&g_lock);
    // Sequence handles never repeat within a process. A stale pre-reset reservation cannot commit
    // a later request after reset, even if its exact lifecycle tuple is reused.
}

} // namespace sunrise::server::gameplay::physics::host::mission_signal_queue
