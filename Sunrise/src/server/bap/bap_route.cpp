#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>

#include "../../client/hooks/network/investment/investment_derived_rebuild.h"
#include "../../core/logging/log.h"
#include "../../state/matchmaking/matchmaking_state.h"
#include "../../state/progression/seasonal_experience.h"
#include "encrypted/bap_connection_publication.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::server::bap {
namespace {

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<Session, kSessionCount> g_sessions{};
Scratch g_scratch{};
std::array<WorldRewardRequest, kWorldRewardQueueCapacity> g_worldRewards{};
std::size_t g_worldRewardHead{};
std::size_t g_worldRewardCount{};
/** Measured lifetime of the native item-acquisition flyout. */
constexpr std::uint64_t kAcquisitionPresentationHoldMs = 8'000;
constexpr std::uint8_t kWorldRewardFailureLimit = 8;

[[nodiscard]] bool has_active_family4_peer() noexcept {
    return std::any_of(g_sessions.begin(), g_sessions.end(), [](const Session& session) {
        return session.id != 0 && session.authenticated && session.queuez.family4Active;
    });
}

void pop_world_reward() noexcept {
    g_worldRewards[g_worldRewardHead] = {};
    g_worldRewardHead = (g_worldRewardHead + 1) % g_worldRewards.size();
    --g_worldRewardCount;
}

[[nodiscard]] bool commit_world_reward(const WorldRewardRequest& request) noexcept {
    if (request.kind == WorldRewardKind::item) {
        state::PendingItemAcquisition acquisition{};
        return state::prepare_item_acquisition_for_item(request.itemDefinitionIndex, acquisition)
               && state::commit_item_acquisition(acquisition);
    }
    state::PendingProfileItemAcquisition acquisition{};
    return state::prepare_profile_item_acquisition_for_item(
               request.itemDefinitionIndex, request.quantity, acquisition)
           && state::commit_profile_item_acquisition(acquisition);
}

[[nodiscard]] bool enqueue_world_reward(WorldRewardRequest request) noexcept {
    if (!has_active_family4_peer()) {
        while (g_worldRewardCount != 0) {
            if (!commit_world_reward(g_worldRewards[g_worldRewardHead])) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=world_reward stage=direct result=drop");
            }
            pop_world_reward();
        }
        return commit_world_reward(request);
    }
    if (g_worldRewardCount == g_worldRewards.size()) {
        const bool committed = commit_world_reward(g_worldRewards[g_worldRewardHead]);
        if (committed) {
            arm_account_resync_everywhere();
        }
        pop_world_reward();
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         committed ? "ev=world_reward stage=queue_full result=direct"
                                   : "ev=world_reward stage=queue_full result=drop");
    }
    g_worldRewards[(g_worldRewardHead + g_worldRewardCount) % g_worldRewards.size()] = request;
    ++g_worldRewardCount;
    return true;
}

/** Arms every other active peer after one shared-account transaction is published. */
void publish_account_mutation(Session& origin) noexcept {
    origin.accountMutationPublished = false;
    arm_account_resync_elsewhere(origin);
}

/** @param id Nonzero connection id. @return Matching open session, or null. */
[[nodiscard]] Session* session_for(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return nullptr;
    }
    auto& session = g_sessions[id - 1];
    return session.id == id ? &session : nullptr;
}

/** @param session Its secrets and identity are wiped. */
void clear_session(Session& session) noexcept {
    SecureZeroMemory(&session, sizeof session);
    // Zeroing is not the cleared state: `advertisedRegion` is -1 and zero is a real region.
    session.activity = {};
}

/**
 * Releases an authenticated session's optional matchmaking context.
 * @param session Open session that may not have finished server hello.
 * @return True when there was no context, or the active generation was released.
 */
[[nodiscard]] bool release_matchmaking_context(Session& session) noexcept {
    if (session.matchmakingContext.generation == state::matchmaking::kInvalidGeneration) {
        session.matchmakingContext = {};
        return true;
    }
    if (!state::matchmaking::release_context(session.matchmakingContext)) {
        return false;
    }
    session.matchmakingContext = {};
    return true;
}

/** @param id Session-slot id. @return True when the slot is opened. */
[[nodiscard]] bool open_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    if (session.id != 0) {
        encrypted::release_activity_connection(session);
    }
    clear_session(session);
    session.id = id;
    return true;
}

/** @param id Session-slot id. @return True when the slot is cleared. */
[[nodiscard]] bool close_session(std::uint32_t id) noexcept {
    if (id == 0 || id > g_sessions.size()) {
        return false;
    }
    auto& session = g_sessions[id - 1];
    if (session.id != 0 && !release_matchmaking_context(session)) {
        return false;
    }
    if (session.id != 0) {
        encrypted::release_activity_connection(session);
    }
    clear_session(session);
    return true;
}

/**
 * Routes one validated frame through its connection-owned session.
 * @param request Frame event and caller-owned buffers.
 * @param response Receives encoded response size.
 * @return True when the frame is valid and its service is handled.
 */
[[nodiscard]] bool consume_frame(const client::network::BapRequest& request,
                                 client::network::BapResponse& response) noexcept {
    middleware::bap::OuterFrame frame;
    if (!middleware::bap::parse_frame(request.frame, frame)) {
        return false;
    }
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    bool handled = false;
    if (frame.frameType == middleware::bap::FrameType::encrypted) {
        handled = encrypted::consume(*session, g_scratch, frame, request.response, response.size);
    } else {
        handled = plaintext::consume(*session, g_scratch, frame, request.response, response.size);
    }
    if (!handled) {
        return false;
    }
    if (frame.frameType == middleware::bap::FrameType::encrypted
        && session->accountMutationPublished) {
        publish_account_mutation(*session);
    }
    // A frame response can carry one already-due push in the same bounded socket write.
    bool touchesScratch = true;
    std::size_t deferred = 0;
    if (response.size < request.response.size()
        && encrypted::consume_deferred(*session,
                                       g_scratch,
                                       request.response.subspan(response.size),
                                       deferred,
                                       touchesScratch)) {
        response.size += deferred;
    }
    return true;
}

/**
 * Services one timed poll for a session that may owe a deferred push.
 * @param request Poll event and caller-owned output buffer.
 * @param response Receives encoded notification size.
 * @param touchesScratch Set when the attempt reaches a scratch buffer.
 * @return True when a notification is published.
 */
[[nodiscard]] bool consume_poll(const client::network::BapRequest& request,
                                client::network::BapResponse& response,
                                bool& touchesScratch) noexcept {
    auto* session = session_for(request.connectionId);
    if (session == nullptr) {
        return false;
    }
    // The purchase response carries the Family-4 ownership rows. Refresh Family 5 only after the
    // client has consumed that response, so derived artifact state never mixes adjacent purchases.
    if (session->artifactRefreshArmed) {
        const state::Family5State family = state::investment_snapshot().family5;
        if (client::hooks::network::investment::publish_live_family5(family)) {
            session->artifactRefreshArmed = false;
        }
    }
    return encrypted::consume_deferred(
        *session, g_scratch, request.response, response.size, touchesScratch);
}

} // namespace

void arm_account_resync_elsewhere(Session& origin) noexcept {
    for (auto& peer : g_sessions) {
        if (&peer != &origin && peer.id != 0 && peer.authenticated && peer.queuez.family4Active) {
            peer.accountResyncArmed = true;
        }
    }
}

/** Arms every active peer to re-read the account, including the origin. */
void arm_account_resync_everywhere() noexcept {
    for (auto& peer : g_sessions) {
        if (peer.id == 0 || !peer.authenticated || !peer.queuez.family4Active) {
            continue;
        }
        peer.accountResyncArmed = true;
    }
}

void arm_acquisition_presentation_hold(Session& session) noexcept {
    const std::uint64_t now = GetTickCount64();
    if (now >= session.acquisitionPresentationUntilTick) {
        session.acquisitionPresentationRows = {};
        session.acquisitionPresentationRowCount = 0;
    }
    session.acquisitionPresentationUntilTick =
        (std::max)(session.acquisitionPresentationUntilTick, now + kAcquisitionPresentationHoldMs);
}

bool arm_world_item_acquisition(std::uint16_t itemDefinitionIndex) noexcept {
    return enqueue_world_reward({1, itemDefinitionIndex, WorldRewardKind::item});
}

bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                        std::int32_t quantity) noexcept {
    if (quantity <= 0) {
        return false;
    }
    return enqueue_world_reward({quantity, itemDefinitionIndex, WorldRewardKind::profileItem});
}

bool current_world_reward(WorldRewardRequest& request) noexcept {
    if (g_worldRewardCount == 0) {
        request = {};
        return false;
    }
    request = g_worldRewards[g_worldRewardHead];
    return true;
}

void complete_world_reward() noexcept {
    if (g_worldRewardCount == 0) {
        return;
    }
    pop_world_reward();
}

void fail_world_reward_attempt() noexcept {
    if (g_worldRewardCount == 0
        || ++g_worldRewards[g_worldRewardHead].failures < kWorldRewardFailureLimit) {
        return;
    }
    const bool committed = commit_world_reward(g_worldRewards[g_worldRewardHead]);
    pop_world_reward();
    if (committed) {
        arm_account_resync_everywhere();
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     committed ? "ev=world_reward stage=retry_limit result=direct"
                               : "ev=world_reward stage=retry_limit result=drop");
}

bool arm_seasonal_experience_presentation(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    for (auto& peer : g_sessions) {
        if (peer.id == 0 || !peer.authenticated || !peer.queuez.family4Active
            || peer.pendingSeasonalExperienceAmount
                   > (std::numeric_limits<std::int32_t>::max)() - amount) {
            continue;
        }
        if (!state::progression::seasonal_experience::grant(amount)) {
            return false;
        }
        (void)state::refresh_artifact_progression();
        peer.pendingSeasonalExperienceAmount += amount;
        peer.pendingSeasonalExperienceFailures = 0;
        return true;
    }
    return false;
}

/** Applies one serialized BAP connection lifecycle event. */
bool consume(const client::network::BapRequest& request,
             client::network::BapResponse& response) noexcept {
    response = {};
    AcquireSRWLockExclusive(&g_lock);
    bool success = false;
    // Polls report whether they reached scratch.
    bool touchesScratch = request.event != client::network::BapEvent::poll;
    // Hold the session lock across cryptographic counter reads and updates.
    switch (request.event) {
    case client::network::BapEvent::open:
        success = open_session(request.connectionId);
        break;
    case client::network::BapEvent::frame:
        success = consume_frame(request, response);
        break;
    case client::network::BapEvent::poll:
        success = consume_poll(request, response, touchesScratch);
        break;
    case client::network::BapEvent::close:
        success = close_session(request.connectionId);
        break;
    }
    // Decrypted frames can contain runtime-only keys or tokens, so scratch never outlives the call.
    if (touchesScratch) {
        SecureZeroMemory(&g_scratch, sizeof g_scratch);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return success;
}

/** Securely erases every connection-owned nonce and transform buffer. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    while (g_worldRewardCount != 0) {
        if (!commit_world_reward(g_worldRewards[g_worldRewardHead])) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=world_reward stage=shutdown result=drop");
        }
        pop_world_reward();
    }
    for (auto& session : g_sessions) {
        if (session.id != 0
            && session.matchmakingContext.generation != state::matchmaking::kInvalidGeneration) {
            // State erases runtime descriptors before the opaque association is cleared.
            (void)state::matchmaking::release_context(session.matchmakingContext);
        }
        if (session.id != 0) {
            encrypted::release_activity_connection(session);
        }
    }
    SecureZeroMemory(g_sessions.data(), sizeof g_sessions);
    g_worldRewards = {};
    g_worldRewardHead = 0;
    g_worldRewardCount = 0;
    SecureZeroMemory(&g_scratch, sizeof g_scratch);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::bap
