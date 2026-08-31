#include <array>
#include <cassert>

#include "server/gameplay/mission/content_step_queue.h"

namespace mission = sunrise::server::gameplay::mission;

mission::QueuedContentStep stage_and_publish(std::uint64_t activitySessionId,
                                             std::uint64_t hostGeneration,
                                             std::uint64_t bindingGeneration,
                                             mission::ContentStepTicket ticket) {
    mission::QueuedContentStep staged{};
    assert(mission::stage_content_step(activitySessionId,
                                       hostGeneration,
                                       bindingGeneration,
                                       staged));
    assert(staged.ticket.value == ticket.value && staged.staged && !staged.published);
    mission::QueuedContentStep committed{};
    assert(mission::commit_staged_content_step(ticket, committed));
    assert(committed.ticket.value == ticket.value && !committed.staged && committed.published);
    return committed;
}

int main() {
    mission::reset_content_steps();
    mission::ContentStepIntent intent{};
    intent.commandId = 7;
    intent.stepId = 11;
    intent.kind = mission::ContentStepKind::glimmerSite0ShipSpawn;

    mission::ContentStepTicket ticket{};
    assert(!mission::reserve_content_step(0, 2, 3, intent, ticket));
    assert(mission::reserve_content_step(1, 2, 3, intent, ticket));
    assert(ticket.value != 0);

    mission::ContentStepTicket duplicate{};
    assert(mission::reserve_content_step(1, 2, 3, intent, duplicate));
    assert(duplicate.value == ticket.value);
    mission::ContentStepIntent conflicting = intent;
    conflicting.stepId = 99;
    assert(!mission::reserve_content_step(1, 2, 3, conflicting, duplicate));
    assert(duplicate.value == 0);

    mission::QueuedContentStep queued{};
    assert(!mission::peek_content_step(1, 4, 3, queued));
    assert(!mission::peek_content_step(1, 2, 4, queued));
    assert(!mission::peek_content_step(1, 2, 3, queued));
    assert(!mission::stage_content_step(1, 2, 3, queued));
    assert(mission::commit_content_step(ticket));
    assert(mission::commit_content_step(ticket)); // exact commit is idempotent before publication
    assert(mission::peek_content_step(1, 2, 3, queued));
    assert(queued.ticket.value == ticket.value && queued.bindingGeneration == 3
           && queued.producerReady && !queued.published && !queued.settled && !queued.observed
           && queued.observationGeneration == 0
           && queued.stage == mission::ContentStepStage::publish);
    mission::ContentStepObservation observed{};
    assert(!mission::claim_content_step_observation(1, 2, 3, intent.stepId, 1, observed));
    assert(observed.ticket.value == 0);
    assert(!mission::claim_content_step_observation(1, 2, 3, intent.stepId, 0, observed));
    assert(!mission::settle_content_step(ticket));
    static_cast<void>(stage_and_publish(1, 2, 3, ticket));
    assert(mission::peek_content_step(1, 2, 3, queued)
           && queued.baselineReady && queued.baselineGeneration == 1);
    assert(!mission::claim_content_step_observation(1, 2, 3, intent.stepId, 1, observed));
    assert(!mission::claim_content_step_observation(2, 2, 3, intent.stepId, 4, observed));
    assert(!mission::claim_content_step_observation(1, 2, 3, 99, 4, observed));
    assert(mission::claim_content_step_observation(1, 2, 3, intent.stepId, 4, observed));
    assert(observed.ticket.value == ticket.value && observed.activitySessionId == 1
           && observed.hostGeneration == 2 && observed.bindingGeneration == 3
           && observed.stepId == intent.stepId && observed.generation == 4);
    assert(!mission::claim_content_step_observation(1, 2, 3, intent.stepId, 4, observed));
    assert(observed.ticket.value == 0);
    assert(mission::peek_content_step(1, 2, 3, queued) && queued.observed
           && queued.observationGeneration == 4);
    assert(mission::settle_content_step(ticket));
    assert(!mission::peek_content_step(1, 2, 3, queued));

    intent.commandId = 8;
    intent.kind = mission::ContentStepKind::glimmerIntro;
    mission::ContentStepTicket introTicket{};
    assert(mission::reserve_content_step(1, 2, 3, intent, introTicket));
    mission::ContentStepIntent enterIntent = intent;
    enterIntent.commandId = 9;
    enterIntent.stepId = 12;
    enterIntent.kind = mission::ContentStepKind::glimmerSite0Enter;
    mission::ContentStepTicket enterTicket{};
    assert(mission::reserve_content_step(1, 2, 3, enterIntent, enterTicket));
    assert(introTicket.value < enterTicket.value);
    assert(!mission::peek_content_step(1, 2, 3, queued));
    assert(mission::commit_content_step(introTicket));
    assert(mission::commit_content_step(enterTicket));
    assert(mission::peek_content_step(1, 2, 3, queued));
    assert(queued.ticket.value == introTicket.value
           && queued.intent.kind == mission::ContentStepKind::glimmerIntro);
    static_cast<void>(stage_and_publish(1, 2, 3, introTicket));
    assert(mission::settle_content_step(introTicket));
    assert(mission::peek_content_step(1, 2, 3, queued));
    assert(queued.ticket.value == enterTicket.value
           && queued.intent.kind == mission::ContentStepKind::glimmerSite0Enter);
    static_cast<void>(stage_and_publish(1, 2, 3, enterTicket));
    assert(mission::settle_content_step(enterTicket));
    assert(!mission::peek_content_step(1, 2, 3, queued));

    // A ship receipt may arrive after outbound settlement. The settled spawn ticket remains
    // claimable but no longer blocks the next outbound content step.
    mission::ContentStepIntent lateSpawn{};
    lateSpawn.commandId = 10;
    lateSpawn.stepId = 11;
    lateSpawn.kind = mission::ContentStepKind::glimmerSite0ShipSpawn;
    mission::ContentStepTicket lateTicket{};
    assert(mission::reserve_content_step(1, 2, 3, lateSpawn, lateTicket));
    assert(mission::commit_content_step(lateTicket));
    static_cast<void>(stage_and_publish(1, 2, 3, lateTicket));
    assert(mission::settle_content_step(lateTicket));
    assert(!mission::settle_content_step(lateTicket));
    assert(!mission::reserve_content_step(1, 2, 3, lateSpawn, duplicate));
    assert(duplicate.value == 0);
    assert(!mission::peek_content_step(1, 2, 3, queued));
    assert(mission::claim_content_step_observation(1, 2, 3, lateSpawn.stepId, 5, observed));
    assert(observed.ticket.value == lateTicket.value);
    assert(!mission::claim_content_step_observation(1, 2, 3, lateSpawn.stepId, 5, observed));

    // A later loop needs a strictly newer object generation. Duplicate and out-of-order Sense
    // frames cannot consume its ticket.
    lateSpawn.commandId = 11;
    mission::ContentStepTicket laterTicket{};
    assert(mission::reserve_content_step(1, 2, 3, lateSpawn, laterTicket));
    assert(mission::commit_content_step(laterTicket));
    static_cast<void>(stage_and_publish(1, 2, 3, laterTicket));
    assert(mission::settle_content_step(laterTicket));
    assert(!mission::claim_content_step_observation(1, 2, 3, lateSpawn.stepId, 4, observed));
    assert(!mission::claim_content_step_observation(1, 2, 3, lateSpawn.stepId, 5, observed));
    assert(mission::claim_content_step_observation(1, 2, 3, lateSpawn.stepId, 6, observed));
    assert(observed.ticket.value == laterTicket.value);

    // Generation cursors are scoped to the exact retained State and private-client lifetime.
    lateSpawn.commandId = 12;
    mission::ContentStepTicket reboundTicket{};
    assert(mission::reserve_content_step(1, 20, 30, lateSpawn, reboundTicket));
    assert(mission::commit_content_step(reboundTicket));
    assert(!mission::claim_content_step_observation(1, 20, 30, lateSpawn.stepId, 1, observed));
    static_cast<void>(stage_and_publish(1, 20, 30, reboundTicket));
    assert(mission::settle_content_step(reboundTicket));
    assert(!mission::claim_content_step_observation(1, 2, 3, lateSpawn.stepId, 7, observed));
    assert(!mission::claim_content_step_observation(1, 20, 30, lateSpawn.stepId, 1, observed));
    assert(mission::claim_content_step_observation(1, 20, 30, lateSpawn.stepId, 2, observed));
    assert(observed.ticket.value == reboundTicket.value && observed.generation == 2);

    // A first-use dropship object may have no pre-publication generation. Its first exact-lifetime
    // post-publication snapshot establishes a baseline but cannot claim the ticket. Only a second,
    // strictly newer generation is a receipt, including when outbound settlement happened first.
    lateSpawn.commandId = 16;
    mission::ContentStepTicket noBaselineTicket{};
    assert(mission::reserve_content_step(1, 40, 50, lateSpawn, noBaselineTicket));
    assert(mission::commit_content_step(noBaselineTicket));
    static_cast<void>(stage_and_publish(1, 40, 50, noBaselineTicket));
    assert(mission::settle_content_step(noBaselineTicket));
    assert(!mission::claim_content_step_observation(1, 40, 50, lateSpawn.stepId, 1, observed));
    assert(observed.ticket.value == 0);
    assert(mission::claim_content_step_observation(1, 40, 50, lateSpawn.stepId, 2, observed));
    assert(observed.ticket.value == noBaselineTicket.value && observed.generation == 2);
    assert(!mission::claim_content_step_observation(1, 40, 50, lateSpawn.stepId, 2, observed));

    // A later loop supersedes one settled, unobserved spawn ticket for the same exact step. This
    // bounds retained capacity without letting the stale handle publish or settle the replacement.
    mission::ContentStepIntent orphan = lateSpawn;
    orphan.commandId = 17;
    orphan.stepId = 66;
    assert(!mission::claim_content_step_observation(1, 60, 70, orphan.stepId, 1, observed));
    mission::ContentStepTicket orphanTicket{};
    assert(mission::reserve_content_step(1, 60, 70, orphan, orphanTicket));
    assert(mission::commit_content_step(orphanTicket));
    static_cast<void>(stage_and_publish(1, 60, 70, orphanTicket));
    assert(mission::settle_content_step(orphanTicket));
    mission::ContentStepIntent replacement = orphan;
    replacement.commandId = 18;
    mission::ContentStepTicket replacementTicket{};
    assert(mission::reserve_content_step(1, 60, 70, replacement, replacementTicket));
    assert(replacementTicket.value != orphanTicket.value);
    assert(mission::commit_content_step(replacementTicket));
    mission::QueuedContentStep stagedReplacement{};
    assert(mission::stage_content_step(1, 60, 70, stagedReplacement));
    assert(stagedReplacement.ticket.value == replacementTicket.value);
    mission::QueuedContentStep staleCommit{};
    assert(!mission::commit_staged_content_step(orphanTicket, staleCommit));
    assert(mission::commit_staged_content_step(replacementTicket, stagedReplacement));
    assert(mission::settle_content_step(replacementTicket));
    assert(mission::claim_content_step_observation(1, 60, 70, replacement.stepId, 2, observed));
    assert(observed.ticket.value == replacementTicket.value);

    // Compiler-visible non-spawn placeholders never become observation-bearing tickets.
    mission::ContentStepIntent unsupported = lateSpawn;
    unsupported.commandId = 13;
    unsupported.stepId = 44;
    unsupported.kind = mission::ContentStepKind::glimmerIntro;
    mission::ContentStepTicket unsupportedTicket{};
    assert(mission::reserve_content_step(1, 2, 3, unsupported, unsupportedTicket));
    assert(mission::commit_content_step(unsupportedTicket));
    static_cast<void>(stage_and_publish(1, 2, 3, unsupportedTicket));
    assert(mission::settle_content_step(unsupportedTicket));
    assert(!mission::claim_content_step_observation(1, 2, 3, unsupported.stepId, 7, observed));

    intent.commandId = 14;
    intent.stepId = 13;
    assert(mission::reserve_content_step(1, 2, 3, intent, ticket));
    mission::cancel_content_steps(1, 2, 4);
    assert(!mission::peek_content_step(1, 2, 3, queued));
    assert(mission::commit_content_step(ticket));
    assert(mission::peek_content_step(1, 2, 3, queued));
    mission::cancel_content_steps(1, 2, 3);
    assert(!mission::peek_content_step(1, 2, 3, queued));

    // A producer rollback can remove only a provisional ticket. Once committed, the ticket is
    // visible to BAP and cancellation fails closed even before publication is marked.
    mission::ContentStepIntent rollbackIntent = intent;
    rollbackIntent.commandId = 19;
    rollbackIntent.stepId = 77;
    mission::ContentStepTicket rollbackTicket{};
    assert(mission::reserve_content_step(1, 80, 90, rollbackIntent, rollbackTicket));
    assert(mission::cancel_content_step(rollbackTicket));
    assert(!mission::cancel_content_step(rollbackTicket));
    assert(!mission::peek_content_step(1, 80, 90, queued));
    rollbackIntent.commandId = 20;
    assert(mission::reserve_content_step(1, 80, 90, rollbackIntent, rollbackTicket));
    assert(!mission::peek_content_step(1, 80, 90, queued));
    assert(mission::commit_content_step(rollbackTicket));
    assert(mission::peek_content_step(1, 80, 90, queued));
    assert(!mission::cancel_content_step(rollbackTicket));
    static_cast<void>(stage_and_publish(1, 80, 90, rollbackTicket));
    assert(mission::settle_content_step(rollbackTicket));

    // Lifecycle cancellation cannot erase a BAP-pinned body. Discard resolves the pin and removes
    // the cancel-pending slot because the copied body never reached the caller.
    rollbackIntent.commandId = 21;
    rollbackIntent.stepId = 78;
    assert(mission::reserve_content_step(1, 80, 90, rollbackIntent, rollbackTicket));
    assert(mission::commit_content_step(rollbackTicket));
    assert(mission::stage_content_step(1, 80, 90, queued));
    assert(queued.ticket.value == rollbackTicket.value && queued.staged);
    mission::ContentStepIntent followingIntent = rollbackIntent;
    followingIntent.commandId = 23;
    followingIntent.stepId = 79;
    mission::ContentStepTicket followingTicket{};
    assert(mission::reserve_content_step(1, 80, 90, followingIntent, followingTicket));
    assert(mission::commit_content_step(followingTicket));
    mission::QueuedContentStep blockedByPin{};
    assert(!mission::stage_content_step(1, 80, 90, blockedByPin));
    mission::cancel_content_steps(1, 80, 90);
    assert(mission::peek_content_step(1, 80, 90, queued));
    assert(queued.staged && queued.cancelPending);
    assert(mission::discard_staged_content_step(rollbackTicket));
    assert(!mission::peek_content_step(1, 80, 90, queued));

    // Reset also defers an in-flight pin. If the caller already received that body, staged commit
    // still resolves the exact old ticket, reports cancellation, and then removes it.
    rollbackIntent.commandId = 22;
    assert(mission::reserve_content_step(1, 80, 90, rollbackIntent, rollbackTicket));
    assert(mission::commit_content_step(rollbackTicket));
    assert(mission::stage_content_step(1, 80, 90, queued));
    mission::reset_content_steps();
    assert(mission::peek_content_step(1, 80, 90, queued));
    assert(queued.staged && queued.cancelPending);
    mission::QueuedContentStep canceledCommit{};
    assert(mission::commit_staged_content_step(rollbackTicket, canceledCommit));
    assert(canceledCommit.published && canceledCommit.cancelPending);
    assert(!mission::peek_content_step(1, 80, 90, queued));
    assert(!mission::settle_content_step(rollbackTicket));

    // Reset clears unpinned storage but never rewinds the process-local handle sequence.
    const mission::ContentStepTicket staleTicket = ticket;
    mission::reset_content_steps();
    intent.commandId = 15;
    mission::ContentStepTicket postResetTicket{};
    assert(mission::reserve_content_step(1, 2, 3, intent, postResetTicket));
    assert(postResetTicket.value > staleTicket.value);
    mission::QueuedContentStep stalePublish{};
    assert(!mission::commit_staged_content_step(staleTicket, stalePublish));
    assert(!mission::peek_content_step(1, 2, 3, queued));
    assert(mission::commit_content_step(postResetTicket));
    assert(mission::peek_content_step(1, 2, 3, queued) && !queued.published);
    static_cast<void>(stage_and_publish(1, 2, 3, postResetTicket));
    assert(mission::settle_content_step(postResetTicket));

    // Full capacity reports backpressure without disturbing any reserved command. Releasing one
    // unpublished ticket makes exactly one slot available for a retry.
    std::array<mission::ContentStepTicket, 32> capacityTickets{};
    mission::ContentStepIntent capacityIntent{};
    capacityIntent.kind = mission::ContentStepKind::glimmerIntro;
    for (std::size_t index = 0; index < capacityTickets.size(); ++index) {
        capacityIntent.commandId = 100 + index;
        capacityIntent.stepId = 1'000 + index;
        assert(mission::reserve_content_step(101, 102, 103, capacityIntent,
                                             capacityTickets[index]));
    }
    capacityIntent.commandId = 1'000;
    capacityIntent.stepId = 2'000;
    mission::ContentStepTicket overflowTicket{};
    assert(!mission::reserve_content_step(101, 102, 103, capacityIntent, overflowTicket));
    assert(overflowTicket.value == 0);
    assert(mission::cancel_content_step(capacityTickets[0]));
    assert(mission::reserve_content_step(101, 102, 103, capacityIntent, overflowTicket));
    assert(overflowTicket.value != 0);
    mission::cancel_content_steps(101, 102, 103);

    // Orphan observations can occupy every cursor without permanently stalling a later exact
    // published ticket. Its first observation reclaims only an unreferenced cursor and establishes
    // the baseline; the second strictly newer generation claims it.
    mission::reset_content_steps();
    for (std::uint64_t index = 0; index < 32; ++index) {
        assert(!mission::claim_content_step_observation(
            200, 201, 202, 3'000 + index, 1, observed));
    }
    mission::ContentStepIntent exhaustedCursorIntent{};
    exhaustedCursorIntent.commandId = 4'000;
    exhaustedCursorIntent.stepId = 4'001;
    exhaustedCursorIntent.kind = mission::ContentStepKind::glimmerSite0ShipSpawn;
    mission::ContentStepTicket exhaustedCursorTicket{};
    assert(mission::reserve_content_step(
        200, 201, 202, exhaustedCursorIntent, exhaustedCursorTicket));
    assert(mission::commit_content_step(exhaustedCursorTicket));
    static_cast<void>(stage_and_publish(200, 201, 202, exhaustedCursorTicket));
    assert(mission::settle_content_step(exhaustedCursorTicket));
    assert(!mission::claim_content_step_observation(
        200, 201, 202, exhaustedCursorIntent.stepId, 1, observed));
    assert(observed.ticket.value == 0);
    assert(mission::claim_content_step_observation(
        200, 201, 202, exhaustedCursorIntent.stepId, 2, observed));
    assert(observed.ticket.value == exhaustedCursorTicket.value && observed.generation == 2);
}
