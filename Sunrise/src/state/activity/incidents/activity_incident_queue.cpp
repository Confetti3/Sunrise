#include <Windows.h>

#include <array>
#include <atomic>

#include "runtime.h"

namespace sunrise::state::activity::incidents {
namespace {

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Observation, kObservationCapacity> g_queue{};
std::size_t g_head{};
std::size_t g_size{};
std::uint64_t g_nextSequence{1};
std::atomic_uint64_t g_dropped{};

} // namespace

/** Publishes one copied observation without blocking the activity-message route. */
bool publish(Observation observation) noexcept {
    if (observation.extraTargetCount > observation.extraTargets.size()) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (g_size == g_queue.size()) {
        g_queue[g_head] = {};
        g_head = (g_head + 1) % g_queue.size();
        --g_size;
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    observation.sequence = g_nextSequence++;
    observation.observedAtTickMs = GetTickCount64();
    observation.droppedBefore = g_dropped.load(std::memory_order_relaxed);
    const std::size_t tail = (g_head + g_size) % g_queue.size();
    g_queue[tail] = observation;
    ++g_size;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Removes the oldest queued observation. */
bool try_pop(Observation& observation) noexcept {
    observation = {};
    AcquireSRWLockExclusive(&g_lock);
    if (g_size == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    observation = g_queue[g_head];
    g_queue[g_head] = {};
    g_head = (g_head + 1) % g_queue.size();
    --g_size;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Discards observations that predate a new script-host connection. */
std::size_t discard_pending() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const std::size_t discarded = g_size;
    g_queue = {};
    g_head = 0;
    g_size = 0;
    if (discarded != 0) {
        g_dropped.fetch_add(static_cast<std::uint64_t>(discarded), std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return discarded;
}

/** @return Total observations dropped by the bounded relay. */
std::uint64_t dropped_count() noexcept {
    return g_dropped.load(std::memory_order_relaxed);
}

} // namespace sunrise::state::activity::incidents
