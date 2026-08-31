#include <array>
#include <cassert>

#include "server/gameplay/mission/content_step_queue.h"
#include "server/gameplay/physics/host/mission_signal_queue.h"

namespace mission = sunrise::server::gameplay::mission;
namespace queue = sunrise::server::gameplay::physics::host::mission_signal_queue;

void publish_content(std::uint64_t activitySessionId,
                     std::uint64_t hostGeneration,
                     std::uint64_t bindingGeneration,
                     mission::ContentStepTicket ticket) {
    mission::QueuedContentStep staged{};
    assert(mission::stage_content_step(activitySessionId,
                                       hostGeneration,
                                       bindingGeneration,
                                       staged));
    assert(staged.ticket.value == ticket.value && staged.staged);
    mission::QueuedContentStep committed{};
    assert(mission::commit_staged_content_step(ticket, committed));
    assert(committed.published && !committed.staged);
}

int main() {
    queue::reset();
    queue::Request request{};
    assert(!queue::register_scope(0, 2, 3));
    assert(queue::reserve(1, 2, 3, 10, true, false) == 0);
    assert(queue::register_scope(1, 2, 3));
    assert(queue::register_scope(1, 2, 3));

    const std::uint64_t reserved = queue::reserve(1, 2, 3, 10, true, false);
    assert(reserved != 0);
    assert(!queue::peek(1, 2, 3, request));
    int rejectedClaims = 0;
    const auto rejectClaim = [](void* context) noexcept {
        ++*static_cast<int*>(context);
        return false;
    };
    assert(!queue::commit_if(reserved, rejectClaim, &rejectedClaims));
    assert(rejectedClaims == 1);
    assert(!queue::peek(1, 2, 3, request));
    assert(queue::commit(reserved));
    assert(!queue::commit(reserved));
    assert(!queue::peek(1, 2, 4, request));
    assert(queue::peek(1, 2, 3, request));
    assert(request.sequence == reserved && request.id == 10 && request.scoped
           && request.ready && request.occupied);
    assert(queue::consume(reserved));
    assert(!queue::consume(reserved));

    // The queue preserves sequence order. The mission policy can reject and consume an unknown
    // head, after which the next known request becomes visible instead of remaining blocked.
    const std::uint64_t unknown = queue::reserve(1, 2, 3, 99, true, true);
    const std::uint64_t known = queue::reserve(1, 2, 3, 10, true, true);
    assert(unknown != 0 && known > unknown);
    assert(queue::peek(1, 2, 3, request) && request.sequence == unknown);
    assert(queue::consume(unknown));
    assert(queue::peek(1, 2, 3, request) && request.sequence == known);
    assert(queue::consume(known));

    // Scope retirement purges both committed and reserved requests. Rebinding another lifetime
    // cannot see or commit either stale handle.
    const std::uint64_t oldReady = queue::reserve(1, 2, 3, 11, true, true);
    const std::uint64_t oldReserved = queue::reserve(1, 2, 3, 12, true, false);
    assert(oldReady != 0 && oldReserved != 0);
    queue::unregister_scope(1, 2, 3);
    assert(!queue::peek(1, 2, 3, request));
    assert(!queue::commit(oldReserved));
    assert(queue::register_scope(1, 2, 4));
    assert(!queue::peek(1, 2, 4, request));

    // An unscoped console request is visible to any live mission tuple.
    const std::uint64_t global = queue::reserve(0, 0, 0, 20, false, true);
    assert(global != 0);
    assert(queue::peek(50, 60, 70, request) && request.sequence == global);
    assert(queue::consume(global));

    // Exact live scope capacity is bounded to the four world slots.
    queue::reset();
    for (std::uint64_t index = 0; index < 4; ++index) {
        assert(queue::register_scope(100 + index, 200 + index, 300 + index));
    }
    assert(!queue::register_scope(999, 999, 999));
    queue::unregister_scope(101, 201, 301);
    assert(queue::register_scope(999, 999, 999));

    // Sixteen requests fit. The seventeenth applies backpressure without disturbing the head;
    // consuming one request allows one later reservation.
    queue::reset();
    std::array<std::uint64_t, 16> sequences{};
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        sequences[index] = queue::reserve(0, 0, 0, 1'000 + index, false, true);
        assert(sequences[index] != 0);
    }
    assert(queue::reserve(0, 0, 0, 2'000, false, true) == 0);
    assert(queue::peek(1, 1, 1, request) && request.sequence == sequences[0]);
    assert(queue::consume(sequences[0]));
    const std::uint64_t afterBackpressure = queue::reserve(0, 0, 0, 2'000, false, true);
    assert(afterBackpressure > sequences.back());

    // Reset clears storage and scopes but never rewinds handles. A stale reservation cannot commit
    // or consume a later request with an aliased sequence.
    const std::uint64_t stale = queue::reserve(0, 0, 0, 3'000, false, false);
    assert(stale == 0); // capacity is still full
    queue::reset();
    const std::uint64_t preReset = queue::reserve(0, 0, 0, 3'001, false, false);
    assert(preReset != 0);
    queue::reset();
    const std::uint64_t postReset = queue::reserve(0, 0, 0, 3'002, false, false);
    assert(postReset > preReset);
    assert(!queue::commit(preReset));
    assert(queue::commit(postReset));
    assert(queue::peek(9, 9, 9, request) && request.sequence == postReset);
    assert(queue::consume(postReset));

    // The observation claim runs inside signal commit. If scope retirement wins first, the
    // callback is not invoked and the exact content ticket remains claimable after a clean rebind.
    mission::reset_content_steps();
    queue::reset();
    assert(queue::register_scope(500, 600, 700));
    mission::ContentStepIntent spawn{};
    spawn.commandId = 1;
    spawn.stepId = 800;
    spawn.kind = mission::ContentStepKind::glimmerSite0ShipSpawn;
    mission::ContentStepObservation observation{};
    assert(!mission::claim_content_step_observation(500, 600, 700, spawn.stepId, 1,
                                                    observation));
    mission::ContentStepTicket spawnTicket{};
    assert(mission::reserve_content_step(500, 600, 700, spawn, spawnTicket));
    assert(mission::commit_content_step(spawnTicket));
    publish_content(500, 600, 700, spawnTicket);
    assert(mission::settle_content_step(spawnTicket));
    struct ClaimContext final {
        std::uint32_t generation{};
        mission::ContentStepObservation* observation{};
        int calls{};
    } claimContext{2, &observation, 0};
    const auto claimSpawn = [](void* opaque) noexcept {
        ClaimContext& context = *static_cast<ClaimContext*>(opaque);
        ++context.calls;
        return mission::claim_content_step_observation(
            500, 600, 700, 800, context.generation, *context.observation);
    };
    const std::uint64_t spawnSignal = queue::reserve(500, 600, 700, 900, true, false);
    assert(spawnSignal != 0);
    assert(queue::commit_if(spawnSignal, claimSpawn, &claimContext));
    assert(claimContext.calls == 1 && observation.ticket.value == spawnTicket.value);
    assert(queue::peek(500, 600, 700, request) && request.sequence == spawnSignal);
    assert(queue::consume(spawnSignal));

    ++spawn.commandId;
    mission::ContentStepTicket reboundTicket{};
    assert(mission::reserve_content_step(500, 600, 700, spawn, reboundTicket));
    assert(mission::commit_content_step(reboundTicket));
    publish_content(500, 600, 700, reboundTicket);
    assert(mission::settle_content_step(reboundTicket));
    const std::uint64_t retiredSignal = queue::reserve(500, 600, 700, 900, true, false);
    assert(retiredSignal != 0);
    queue::unregister_scope(500, 600, 700);
    claimContext = {3, &observation, 0};
    assert(!queue::commit_if(retiredSignal, claimSpawn, &claimContext));
    assert(claimContext.calls == 0);
    assert(queue::register_scope(500, 600, 700));
    const std::uint64_t reboundSignal = queue::reserve(500, 600, 700, 900, true, false);
    assert(reboundSignal > retiredSignal);
    assert(queue::commit_if(reboundSignal, claimSpawn, &claimContext));
    assert(claimContext.calls == 1 && observation.ticket.value == reboundTicket.value);
    assert(queue::consume(reboundSignal));

    // An enter-command body reserves its exact scoped signal before encode. Successful delivery
    // commits the pinned content ticket inside signal publication; discard releases both pins.
    mission::reset_content_steps();
    queue::reset();
    assert(queue::register_scope(510, 610, 710));
    mission::ContentStepIntent enter{};
    enter.commandId = 10;
    enter.stepId = 810;
    enter.kind = mission::ContentStepKind::glimmerSite0Enter;
    mission::ContentStepTicket enterTicket{};
    assert(mission::reserve_content_step(510, 610, 710, enter, enterTicket));
    assert(mission::commit_content_step(enterTicket));
    mission::QueuedContentStep stagedEnter{};
    assert(mission::stage_content_step(510, 610, 710, stagedEnter));
    assert(stagedEnter.ticket.value == enterTicket.value && stagedEnter.staged);
    const std::uint64_t enterSignal = queue::reserve(510, 610, 710, 910, true, false);
    assert(enterSignal != 0);
    struct PublishContext final {
        mission::ContentStepTicket ticket{};
        mission::QueuedContentStep committed{};
        int calls{};
    } publishContext{enterTicket, {}, 0};
    const auto publishEnter = [](void* opaque) noexcept {
        PublishContext& context = *static_cast<PublishContext*>(opaque);
        ++context.calls;
        return mission::commit_staged_content_step(context.ticket, context.committed);
    };
    assert(queue::commit_if(enterSignal, publishEnter, &publishContext));
    assert(publishContext.calls == 1 && publishContext.committed.published);
    assert(queue::peek(510, 610, 710, request) && request.sequence == enterSignal
           && request.id == 910);
    assert(queue::consume(enterSignal));
    assert(mission::settle_content_step(enterTicket));

    ++enter.commandId;
    mission::ContentStepTicket discardedEnter{};
    assert(mission::reserve_content_step(510, 610, 710, enter, discardedEnter));
    assert(mission::commit_content_step(discardedEnter));
    assert(mission::stage_content_step(510, 610, 710, stagedEnter));
    const std::uint64_t discardedSignal = queue::reserve(510, 610, 710, 910, true, false);
    assert(discardedSignal != 0);
    assert(queue::consume(discardedSignal));
    assert(mission::discard_staged_content_step(discardedEnter));
    assert(mission::stage_content_step(510, 610, 710, stagedEnter));
    assert(stagedEnter.ticket.value == discardedEnter.value);
    assert(mission::discard_staged_content_step(discardedEnter));
    mission::cancel_content_steps(510, 610, 710);

    // If exact scope retirement wins, signal commit does not invoke publication. The roster path
    // must still commit an already delivered immutable ticket, while no stale signal survives.
    ++enter.commandId;
    mission::ContentStepTicket retiredEnter{};
    assert(mission::reserve_content_step(510, 610, 710, enter, retiredEnter));
    assert(mission::commit_content_step(retiredEnter));
    assert(mission::stage_content_step(510, 610, 710, stagedEnter));
    const std::uint64_t retiredEnterSignal = queue::reserve(510, 610, 710, 910, true, false);
    assert(retiredEnterSignal != 0);
    queue::unregister_scope(510, 610, 710);
    publishContext = {retiredEnter, {}, 0};
    assert(!queue::commit_if(retiredEnterSignal, publishEnter, &publishContext));
    assert(publishContext.calls == 0);
    assert(mission::commit_staged_content_step(retiredEnter, publishContext.committed));
    assert(publishContext.committed.published);
    assert(!queue::peek(510, 610, 710, request));
}
