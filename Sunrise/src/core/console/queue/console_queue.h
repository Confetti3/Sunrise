#pragma once

#include <cstddef>
#include <cstdint>

#include "../definition.h"
#include "../parser/console_line_parse.h"

namespace sunrise::core::console::queue {

/**
 * Invocations that may wait at once.
 *
 * A reader submits one line at a time and the drain runs every frame, so the depth only has to
 * absorb a burst from a caller that is not a reader. Past this a submission is refused rather
 * than dropped silently, so the caller learns its work was not taken.
 */
inline constexpr std::size_t kQueueCapacity = 32;

/** Never issued as a ticket, so it doubles as the refusal a full queue reports. */
inline constexpr std::uint64_t kNoTicket = 0;

/**
 * Receives one drained result, on the draining thread.
 * @param ticket Ticket the submission was given.
 * @param result What the invocation reported.
 */
using CompletionCallback = void (*)(std::uint64_t ticket, const Result& result) noexcept;

/**
 * Takes a checked invocation to run on the draining thread.
 *
 * Nothing runs here. A handler reaches module state that other threads own, and this process
 * already hooks rendering and the network, so a handler called on whichever thread happened to
 * submit would be a data race waiting for a second caller. Submitting instead of calling is what
 * lets a future off-thread caller share these handlers without touching them.
 *
 * @param invocation Checked invocation, as `parse_line` produced it.
 * @return Its ticket, or `kNoTicket` when the queue is full.
 */
[[nodiscard]] std::uint64_t submit(const parser::Invocation& invocation) noexcept;

/**
 * Runs every waiting invocation on the calling thread, oldest first.
 *
 * A handler runs with no queue lock held, so it is free to take whatever lock its own module
 * needs without ordering itself against this one. The same holds for the completion and for the
 * registered observer.
 *
 * @param completion Called once per invocation with its result. May be null to discard results.
 * @return Count run.
 */
std::size_t drain(CompletionCallback completion) noexcept;

/**
 * Registers the one observer every drain reports its results to.
 *
 * The completion belongs to whoever called `drain`. This belongs to whoever is waiting for a
 * result but does not drain: a submitter on another thread has no other way to learn that its line
 * finished, and reaching back through the draining caller would point that caller at a subsystem
 * it has no business knowing about.
 *
 * One slot, because one submitter outside the draining thread is what this queue is built for, and
 * a list would need a lifetime this file cannot see the end of. Registering replaces whatever was
 * there.
 *
 * The observer is called after the completion, on the draining thread, with no queue lock held. It
 * sees every ticket, its own and everyone else's, so an observer that cares which is which has to
 * say so itself.
 *
 * @param observer Called once per invocation with its result, or null to stop observing.
 */
void observe(CompletionCallback observer) noexcept;

/** @return Count waiting to run. */
[[nodiscard]] std::size_t pending() noexcept;

/** Drops every waiting invocation without running any. Tickets keep rising. */
void clear() noexcept;

/** Drops waiting work and the observer, and invalidates every in-flight completion. */
void shutdown() noexcept;

} // namespace sunrise::core::console::queue
