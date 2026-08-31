#include "console_queue.h"

#include <Windows.h>

#include <array>
#include <atomic>

#include "../invoke/console_invoke.h"

namespace sunrise::core::console::queue {
namespace {

/** One waiting invocation and the ticket its submitter holds. */
struct Pending {
    parser::Invocation invocation{};
    std::uint64_t ticket{};
    std::uint64_t generation{};
};

std::array<Pending, kQueueCapacity> g_pending{};
/** Index of the oldest waiting entry. */
std::size_t g_head{};
std::size_t g_count{};
/** Last issued ticket. It only ever rises, so no live ticket is ever reused. */
std::uint64_t g_lastTicket{};
/** Generation of submissions that belong to the current queue lifetime. */
std::uint64_t g_generation{1};
/** Enforces the documented single draining thread without holding the queue lock in handlers. */
std::atomic_flag g_draining = ATOMIC_FLAG_INIT;
/**
 * The one observer results are reported to besides the drain's own completion.
 *
 * Null is the ordinary state: whoever drains already has its results, so nothing registers here
 * unless a submitter on another thread is waiting for them.
 */
CompletionCallback g_observer{};
SRWLOCK g_queueLock{SRWLOCK_INIT};

/**
 * Removes the oldest waiting entry.
 * @param output Filled only when one was waiting.
 * @return True when one was removed.
 */
[[nodiscard]] bool take_oldest(Pending& output) noexcept {
    AcquireSRWLockExclusive(&g_queueLock);
    const bool taken = g_count != 0;
    if (taken) {
        output = g_pending[g_head];
        g_pending[g_head] = Pending{};
        g_head = (g_head + 1) % kQueueCapacity;
        --g_count;
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    return taken;
}

/**
 * Confirms an in-flight item still belongs to this queue lifetime and reads its observer.
 * @param generation Generation captured when the item was submitted.
 * @param observer Receives the current observer only while the generation still matches.
 * @return True when shutdown has not invalidated the item.
 */
[[nodiscard]] bool current_completion(std::uint64_t generation,
                                      CompletionCallback& observer) noexcept {
    AcquireSRWLockShared(&g_queueLock);
    const bool current = generation == g_generation;
    observer = current ? g_observer : nullptr;
    ReleaseSRWLockShared(&g_queueLock);
    return current;
}

} // namespace

/** Takes a checked invocation to run on the draining thread. */
std::uint64_t submit(const parser::Invocation& invocation) noexcept {
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_count >= kQueueCapacity) {
        ReleaseSRWLockExclusive(&g_queueLock);
        return kNoTicket;
    }
    ++g_lastTicket;
    if (g_lastTicket == kNoTicket) {
        ++g_lastTicket;
    }
    const std::uint64_t ticket = g_lastTicket;
    g_pending[(g_head + g_count) % kQueueCapacity] =
        Pending{invocation, ticket, g_generation};
    ++g_count;
    ReleaseSRWLockExclusive(&g_queueLock);
    return ticket;
}

/** Runs every waiting invocation on the calling thread, oldest first. */
std::size_t drain(CompletionCallback completion) noexcept {
    if (g_draining.test_and_set(std::memory_order_acquire)) {
        return 0;
    }

    std::size_t ran = 0;
    // The count is read afresh each turn rather than latched, so a handler that submits more work
    // does not have to wait a frame for it. The queue is bounded, so one drain still runs at most
    // the capacity and then yields to the next frame.
    Pending next{};
    while (ran < kQueueCapacity && take_oldest(next)) {
        Result result{};
        invoke::run(next.invocation, result);

        CompletionCallback observer = nullptr;
        if (current_completion(next.generation, observer)) {
            if (completion != nullptr) {
                completion(next.ticket, result);
            }
            if (observer != nullptr) {
                observer(next.ticket, result);
            }
        }
        ++ran;
    }
    g_draining.clear(std::memory_order_release);
    return ran;
}

/** Registers the one observer every drain reports its results to. */
void observe(CompletionCallback observer) noexcept {
    AcquireSRWLockExclusive(&g_queueLock);
    g_observer = observer;
    ReleaseSRWLockExclusive(&g_queueLock);
}

/** @return Count waiting to run. */
std::size_t pending() noexcept {
    AcquireSRWLockShared(&g_queueLock);
    const std::size_t count = g_count;
    ReleaseSRWLockShared(&g_queueLock);
    return count;
}

/** Drops every waiting invocation without running any. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_queueLock);
    g_pending = {};
    g_head = 0;
    g_count = 0;
    ReleaseSRWLockExclusive(&g_queueLock);
}

/** Drops every waiting invocation and the observer, and resets the ticket counter. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_queueLock);
    g_pending = {};
    g_head = 0;
    g_count = 0;
    ++g_generation;
    if (g_generation == 0) {
        ++g_generation;
    }
    // Tickets never restart. Generation invalidation also prevents an in-flight pre-shutdown
    // handler from reporting to a newly installed observer after reinitialization.
    g_observer = nullptr;
    ReleaseSRWLockExclusive(&g_queueLock);
}

} // namespace sunrise::core::console::queue
