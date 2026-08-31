#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "middleware/bap/activity_message/entity_slots.h"
#include "server/bap/runtime.h"
#include "server/bap/internal.h"
#include "server/bap/encrypted/push/activity/activity_entity_slot_republish.h"
#include "server/bap/encrypted/push/activity/activity_roster_atomic.h"
#include "server/bap/encrypted/push/activity/activity_roster_research.h"
#include "state/activity/entity_slots/runtime.h"
#include "state/activity/runtime.h"

namespace bap = sunrise::server::bap;
namespace activity = sunrise::server::bap::encrypted::push::activity;
namespace republish = activity::entity_slot_republish;
namespace slots = sunrise::state::activity::entity_slots;
namespace wire = sunrise::middleware::bap::activity_message::entity_slots;

namespace {
bap::ActivitySnapshot g_snapshot{};
std::array<bap::Session, 2> g_bapSessions{};
bool g_snapshotAvailable{};
bool g_bindingMatches{true};
slots::LeaseMask g_held{};
slots::LeaseMask g_reserved{};
std::uint64_t g_stateRevision{77};
std::uint64_t g_memberKey{88};
std::size_t g_readCalls{};
}

namespace sunrise::server::bap {
bool snapshot_private_activity(ActivitySnapshot& output) noexcept {
    output = g_snapshotAvailable ? g_snapshot : ActivitySnapshot{};
    return g_snapshotAvailable;
}

bool newest_private_activity_matches_locked(const ActivitySnapshot& expected) noexcept {
    return newest_private_activity_matches(g_bapSessions, expected);
}
}

namespace sunrise::state::activity {
bool binding_matches(const SessionBinding&) noexcept { return g_bindingMatches; }
}

namespace sunrise::state::activity::entity_slots {
bool held_mask(const SessionBinding& binding, LeaseMask& held) noexcept {
    ++g_readCalls;
    held = {};
    if (!g_bindingMatches || !state::activity::same_binding(binding, g_snapshot.binding)) {
        return false;
    }
    held = g_held;
    return true;
}
}

void reset_request_state() {
    republish::reset_for_test();
    g_snapshot = {};
    g_bapSessions = {};
    g_snapshotAvailable = false;
    g_bindingMatches = true;
    g_held = {};
    g_reserved = {};
    g_readCalls = 0;
}

void bind_request_source(std::uint64_t sessionId = 0x100,
                         std::uint64_t createdRevision = 10,
                         std::uint64_t bindingGeneration = 20) {
    g_snapshot = {};
    g_snapshot.binding.sessionId = sessionId;
    g_snapshot.binding.createdRevision = createdRevision;
    g_snapshot.binding.destination.activityIndex = 7;
    g_snapshot.bindingGeneration = bindingGeneration;
    g_snapshotAvailable = true;
    bap::Session& current = g_bapSessions[0];
    current.id = 1;
    current.authenticated = true;
    current.activity.role = bap::ActivityClientRole::privateCurrent;
    current.activity.session = g_snapshot.binding;
    current.activity.source = g_snapshot.binding;
    current.activity.bindingGeneration = g_snapshot.bindingGeneration;
}

bap::Session private_session() {
    bap::Session session{};
    session.id = 4;
    session.authenticated = true;
    session.activity.role = bap::ActivityClientRole::privateCurrent;
    session.activity.session = g_snapshot.binding;
    session.activity.source = g_snapshot.binding;
    session.activity.bindingGeneration = g_snapshot.bindingGeneration;
    return session;
}

void test_atomic_pair() {
    std::array<std::byte, 4> nonce{};
    nonce[0] = std::byte{5};
    std::array<std::byte, 12> response{};
    response[0] = std::byte{0xAA};
    response[1] = std::byte{0xBB};
    std::size_t written = 2;
    bool discarded = false;
    assert(activity::detail::append_entity_slot_roster_pair(
        nonce, response, written,
        [&]() noexcept {
            response[written++] = std::byte{0};
            nonce[0] = std::byte{6};
            return true;
        },
        [&]() noexcept {
            response[written++] = std::byte{5};
            nonce[0] = std::byte{7};
            return true;
        },
        [&]() noexcept { discarded = true; }));
    assert(!discarded && written == 4 && response[2] == std::byte{0}
           && response[3] == std::byte{5} && nonce[0] == std::byte{7});

    const auto beforeNonce = nonce;
    written = 2;
    response[2] = std::byte{0xCC};
    response[3] = std::byte{0xDD};
    discarded = false;
    assert(!activity::detail::append_entity_slot_roster_pair(
        nonce, response, written,
        [&]() noexcept {
            response[written++] = std::byte{0};
            nonce[0] = std::byte{8};
            return true;
        },
        [&]() noexcept {
            response[written++] = std::byte{5};
            nonce[0] = std::byte{9};
            return false;
        },
        [&]() noexcept { discarded = true; }));
    assert(discarded && written == 2 && nonce == beforeNonce
           && response[2] == std::byte{} && response[3] == std::byte{});

    bool rosterCalled = false;
    discarded = false;
    assert(!activity::detail::append_entity_slot_roster_pair(
        nonce, response, written,
        [&]() noexcept {
            response[written++] = std::byte{0xEE};
            nonce[0] = std::byte{0x0A};
            return false;
        },
        [&]() noexcept {
            rosterCalled = true;
            return true;
        },
        [&]() noexcept { discarded = true; }));
    assert(!rosterCalled && discarded && written == 2 && nonce == beforeNonce
           && response[2] == std::byte{});
}

void test_authentic_encoder_pair() {
    wire::EntitySlotMask mask{};
    for (std::size_t index = 0; index < mask.size(); ++index) {
        mask[index] = static_cast<std::byte>((index * 37U) & 0xFFU);
    }
    std::array<std::byte, wire::kEncodedSize + 4> response{};
    std::array<std::byte, 4> nonce{};
    std::size_t written = 1;
    assert(activity::detail::append_entity_slot_roster_pair(
        nonce, response, written,
        [&]() noexcept {
            std::size_t encoded = 0;
            const bool ok = wire::encode_entity_slots(
                mask, std::span(response).subspan(written), encoded);
            written += encoded;
            if (ok) nonce[0] = std::byte{1};
            return ok;
        },
        [&]() noexcept {
            response[written++] = std::byte{5};
            nonce[0] = std::byte{2};
            return true;
        },
        []() noexcept {}));
    assert(written == 1 + wire::kEncodedSize + 1 && nonce[0] == std::byte{2});
    assert(std::equal(mask.begin(), mask.end(), response.begin() + 1));
    assert(response[1 + wire::kEncodedSize] == std::byte{5});
}

int main() {
    reset_request_state();
    assert(activity::request_entity_slot_republish() == 0);
    assert(activity::entity_slot_republish_status().noPrivateRejected == 1);

    reset_request_state();
    bind_request_source();
    g_held[0] = std::byte{0x81};
    g_held[511] = std::byte{0x40};
    g_held[1023] = std::byte{0x02};
    g_reserved[1023] = std::byte{0xF0};
    const auto initialHeld = g_held;
    const auto initialReserved = g_reserved;
    const std::uint64_t initialRevision = g_stateRevision;
    const std::uint64_t initialMemberKey = g_memberKey;

    const std::uint64_t first = activity::request_entity_slot_republish();
    assert(first == 1);
    bap::Session session = private_session();
    bap::Session publicSession = session;
    publicSession.activity.role = bap::ActivityClientRole::publicTarget;
    bap::Session noneSession = session;
    noneSession.activity.role = bap::ActivityClientRole::none;
    republish::Selection selected{};
    assert(!republish::select(publicSession, selected));
    assert(!republish::select(publicSession, selected));
    assert(!republish::select(noneSession, selected));
    assert(!republish::select(noneSession, selected));
    auto status = activity::entity_slot_republish_status();
    assert(status.publicRejected == 1 && status.noPrivateRejected == 1);
    assert(republish::select(session, selected));
    assert(selected.token == first && selected.held == initialHeld
           && selected.held[1023] == std::byte{0x02});
    assert(g_held == initialHeld && g_reserved == initialReserved
           && g_stateRevision == initialRevision && g_memberKey == initialMemberKey
           && g_readCalls == 1);

    session.activityRosterStaged.staged = true;
    republish::stage_publication(session, selected);
    assert(session.activityRosterStaged.hasEntitySlotRepublish
           && session.activityRosterStaged.entitySlotRepublishToken == first
           && session.activityRosterStaged.entitySlotRepublishBindingGeneration == 20
           && sunrise::state::activity::same_binding(
               session.activityRosterStaged.entitySlotRepublishBinding, selected.binding));
    const std::uint64_t newer = activity::request_entity_slot_republish();
    assert(newer == 2);
    status = activity::entity_slot_republish_status();
    assert(status.requested == 2 && status.bound == 2 && status.pendingToken == newer
           && status.stagedToken == first);
    // Caller copy happened, then the connection rebound. Delivery still consumes/counts first.
    ++session.activity.bindingGeneration;
    ++session.activity.session.createdRevision;
    session.activity.source = session.activity.session;
    republish::commit_staged_publication(session);
    status = activity::entity_slot_republish_status();
    assert(status.delivered == 1 && status.deliveredToken == first
           && status.pendingToken == newer && !session.activityRosterStaged.hasEntitySlotRepublish);
    republish::commit_staged_publication(session);
    assert(activity::entity_slot_republish_status().delivered == 1);

    session = private_session();
    republish::Selection retry{};
    assert(republish::select(session, retry) && retry.token == newer);
    session.activityRosterStaged.staged = true;
    republish::stage_publication(session, retry);
    republish::discard_staged_publication(session);
    status = activity::entity_slot_republish_status();
    assert(status.discarded == 1 && status.pendingToken == newer
           && !session.activityRosterStaged.hasEntitySlotRepublish);
    assert(republish::select(session, retry) && retry.token == newer);
    session.activityRosterStaged.staged = true;
    republish::stage_publication(session, retry);
    republish::commit_staged_publication(session);
    assert(activity::entity_slot_republish_status().pendingToken == 0);

    const std::uint64_t failed = activity::request_entity_slot_republish();
    assert(failed == 3 && republish::select(session, retry));
    republish::mark_encode_failed(failed);
    status = activity::entity_slot_republish_status();
    assert(status.encodeFailed == 1 && status.pendingToken == failed);
    assert(republish::select(session, retry) && retry.token == failed);
    session.activityRosterStaged = {};
    session.activityRosterStaged.staged = true;
    republish::stage_publication(session, retry);
    republish::commit_staged_publication(session);
    status = activity::entity_slot_republish_status();
    assert(status.pendingToken == 0 && status.deliveredToken == failed);

    // Same session id recreated under an equal connection generation is still stale.
    const std::uint64_t recreated = activity::request_entity_slot_republish();
    assert(recreated == 4);
    bap::Session recreatedSession = private_session();
    ++recreatedSession.activity.session.createdRevision;
    recreatedSession.activity.source = recreatedSession.activity.session;
    assert(!republish::select(recreatedSession, retry));
    status = activity::entity_slot_republish_status();
    assert(status.staleRejected == 1 && status.pendingToken == 0);

    // Repro: A(gen20) captured, but globally newest B(gen21) exists before A can stage.
    const std::uint64_t oldA = activity::request_entity_slot_republish();
    assert(oldA == 5);
    bap::Session oldASession = private_session();
    bap::Session& newestB = g_bapSessions[1];
    newestB.id = 2;
    newestB.authenticated = true;
    newestB.activity.role = bap::ActivityClientRole::privateCurrent;
    newestB.activity.session = g_snapshot.binding;
    newestB.activity.session.sessionId = 0x200;
    newestB.activity.session.createdRevision = 30;
    newestB.activity.source = newestB.activity.session;
    newestB.activity.bindingGeneration = 21;
    bap::ActivitySnapshot expectedA{};
    expectedA.binding = g_snapshot.binding;
    expectedA.bindingGeneration = 20;
    assert(!bap::newest_private_activity_matches(g_bapSessions, expectedA));
    assert(!republish::select(oldASession, retry));
    status = activity::entity_slot_republish_status();
    assert(status.staleRejected == 2 && status.pendingToken == 0);

    test_atomic_pair();
    test_authentic_encoder_pair();
    return 0;
}
