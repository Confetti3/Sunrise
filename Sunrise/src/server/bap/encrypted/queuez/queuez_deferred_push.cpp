#include <Windows.h>

#include <algorithm>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"
#include "../internal.h"
#include "../push/activity/activity_keepalive_push.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

constexpr std::uint8_t kSeasonalExperiencePresentationFailureLimit = 8;

[[nodiscard]] std::span<const queuez::AcquisitionPresentationRow>
active_acquisition_presentation_rows(const Session& session) noexcept {
    if (GetTickCount64() >= session.acquisitionPresentationUntilTick
        || session.acquisitionPresentationRowCount > session.acquisitionPresentationRows.size()) {
        return {};
    }
    return std::span(session.acquisitionPresentationRows)
        .first(session.acquisitionPresentationRowCount);
}

/** Drops only the visual XP notification after repeated failures; the XP is already durable. */
void fail_seasonal_experience_presentation(Session& session) noexcept {
    if (++session.pendingSeasonalExperienceFailures < kSeasonalExperiencePresentationFailureLimit) {
        return;
    }
    session.pendingSeasonalExperienceAmount = 0;
    session.pendingSeasonalExperienceMutationSerial = 0;
    session.pendingSeasonalExperienceFailures = 0;
    bap::arm_account_resync_everywhere();
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=season_xp stage=deferred_presentation result=drop reason=retry_limit");
}

/** Publishes and commits one character-inventory world reward. */
[[nodiscard]] bool consume_world_item_acquisition(const WorldRewardRequest& request,
                                                  Session& session,
                                                  Scratch& scratch,
                                                  std::span<std::byte> response,
                                                  std::size_t& written,
                                                  bool& touchesScratch) noexcept {
    state::PendingItemAcquisition pending{};
    if (!state::prepare_item_acquisition_for_item(request.itemDefinitionIndex, pending)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=prepare");
        bap::fail_world_reward_attempt();
        return false;
    }
    touchesScratch = true;
    queuez::ItemAcquisition acquisition{};
    if (!queuez::stage_item_acquisition(session.queuez,
                                        pending.accountSoid,
                                        pending.characterSoid,
                                        pending.acquiredInstanceSoid,
                                        pending.profileChanged,
                                        acquisition)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=stage");
        bap::fail_world_reward_attempt();
        return false;
    }
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_item_acquisition_notification(scratch,
                                                    acquisition,
                                                    pending,
                                                    std::nullopt,
                                                    active_acquisition_presentation_rows(session),
                                                    state::bap().sessionKey,
                                                    nextSendNonce,
                                                    scratch.framed,
                                                    framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=encode");
        bap::fail_world_reward_attempt();
        return false;
    }
    if (!state::commit_item_acquisition(pending)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_acquisition result=fail reason=commit");
        bap::fail_world_reward_attempt();
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = acquisition.after;
    bap::complete_world_reward();
    bap::arm_account_resync_elsewhere(session);
    bap::arm_acquisition_presentation_hold(session);
    return true;
}

/** Publishes and commits one profile-inventory world reward. */
[[nodiscard]] bool consume_world_profile_item_acquisition(const WorldRewardRequest& request,
                                                          Session& session,
                                                          Scratch& scratch,
                                                          std::span<std::byte> response,
                                                          std::size_t& written,
                                                          bool& touchesScratch) noexcept {
    state::PendingProfileItemAcquisition pending{};
    if (!state::prepare_profile_item_acquisition_for_item(
            request.itemDefinitionIndex, request.quantity, pending)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=prepare");
        bap::fail_world_reward_attempt();
        return false;
    }
    touchesScratch = true;
    queuez::ProfileItemAcquisition acquisition{};
    if (!queuez::stage_profile_item_acquisition(session.queuez,
                                                pending.accountSoid,
                                                pending.acquiredInstanceSoid,
                                                pending.actionSource,
                                                pending.appended,
                                                acquisition)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=stage");
        bap::fail_world_reward_attempt();
        return false;
    }
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_profile_item_acquisition_notification(scratch,
                                                            acquisition,
                                                            pending,
                                                            std::nullopt,
                                                            state::bap().sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=encode");
        bap::fail_world_reward_attempt();
        return false;
    }
    if (!state::commit_profile_item_acquisition(pending)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=world_profile_acquisition result=fail reason=commit");
        bap::fail_world_reward_attempt();
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = acquisition.after;
    bap::complete_world_reward();
    bap::arm_account_resync_elsewhere(session);
    bap::arm_acquisition_presentation_hold(session);
    return true;
}

/** Publishes one non-persistent XP reward row so the native seasonal XP HUD animates. */
[[nodiscard]] bool consume_seasonal_experience_presentation(Session& session,
                                                            Scratch& scratch,
                                                            std::span<std::byte> response,
                                                            std::size_t& written,
                                                            bool& touchesScratch) noexcept {
    if (session.pendingSeasonalExperienceAmount <= 0) {
        return false;
    }
    touchesScratch = true;
    if (session.pendingSeasonalExperienceMutationSerial == 0) {
        std::int32_t mutationSerial = 0;
        if (!state::reserve_selected_character_inventory_serial(mutationSerial)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=season_xp stage=deferred_presentation result=fail reason=serial");
            fail_seasonal_experience_presentation(session);
            return false;
        }
        session.pendingSeasonalExperienceMutationSerial =
            static_cast<std::uint32_t>(mutationSerial) + 1U;
    }
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    if (!push::append_seasonal_experience_notification(
            scratch,
            session.queuez,
            session.pendingSeasonalExperienceAmount,
            static_cast<std::int32_t>(session.pendingSeasonalExperienceMutationSerial - 1U),
            active_acquisition_presentation_rows(session),
            state::bap().sessionKey,
            nextSendNonce,
            scratch.framed,
            framedSize,
            after)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=season_xp stage=deferred_presentation result=fail");
        fail_seasonal_experience_presentation(session);
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    session.pendingSeasonalExperienceAmount = 0;
    session.pendingSeasonalExperienceMutationSerial = 0;
    session.pendingSeasonalExperienceFailures = 0;
    bap::arm_account_resync_elsewhere(session);
    return true;
}

/** Publishes the current account graph to a peer invalidated by another connection. */
[[nodiscard]] bool consume_account_resync(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    if (!session.accountResyncArmed) {
        return false;
    }
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState currentQueuez{};
    if (!push::append_account_resync_notification(scratch,
                                                  session.queuez,
                                                  active_acquisition_presentation_rows(session),
                                                  state::bap().sessionKey,
                                                  nextSendNonce,
                                                  scratch.framed,
                                                  framedSize,
                                                  currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=family4");
        return false;
    }
    if (currentQueuez.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (!push::append_account_resync_appearance_notification(scratch,
                                                                 currentQueuez,
                                                                 state::bap().sessionKey,
                                                                 nextSendNonce,
                                                                 scratch.framed,
                                                                 framedSize,
                                                                 appearanceAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family0");
            return false;
        }
        currentQueuez = appearanceAfter;
    }
    if (currentQueuez.family3Active) {
        queuez::SessionState rosterAfter{};
        if (!push::append_account_resync_roster_notification(scratch,
                                                             currentQueuez,
                                                             state::bap().sessionKey,
                                                             nextSendNonce,
                                                             scratch.framed,
                                                             framedSize,
                                                             rosterAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family3");
            return false;
        }
        currentQueuez = rosterAfter;
    }
    if (framedSize == 0 || framedSize > response.size() || !queuez::valid(currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=output");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = currentQueuez;
    session.accountResyncArmed = false;
    return true;
}

/** Sends the owed banner retry after its delay. */
[[nodiscard]] bool consume_banner_repush(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept {
    if (!session.bannerRepushArmed || session.bannerRepushRoot == 0
        || GetTickCount64() < session.bannerRepushDueTick) {
        return false;
    }
    // Retain the arm until the account has a character to name.
    if (state::account::banner_character_soid(state::account_snapshot()) == 0) {
        return false;
    }
    touchesScratch = true;

    // Reuse the subscription path so its version and the host mirror stay aligned.
    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kBannerFamilyType;
    subscription.familyRootSoid = session.bannerRepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState bannerAfter{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     bannerAfter,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    // The frame is committed here, so the recorded delivery and the arm are committed with it.
    if (valid(bannerAfter)) {
        session.queuez = bannerAfter;
    }
    session.bannerRepushArmed = false;
    return true;
}

/** Refreshes appearance and roster after an asynchronous ability-bucket rebuild. */
[[nodiscard]] bool consume_ability_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    if (!session.abilityRefreshArmed || GetTickCount64() < session.abilityRefreshDueTick) {
        return false;
    }
    // Retain the arm until a family that reads abilities is active.
    if (!session.queuez.family0Active && !session.queuez.family3Active) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState current = session.queuez;
    bool wrote = false;
    if (current.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (push::append_account_resync_appearance_notification(scratch,
                                                                current,
                                                                state::bap().sessionKey,
                                                                nextSendNonce,
                                                                scratch.framed,
                                                                framedSize,
                                                                appearanceAfter)) {
            current = appearanceAfter;
            wrote = true;
        }
    }
    if (current.family3Active) {
        queuez::SessionState rosterAfter{};
        if (push::append_account_resync_roster_notification(scratch,
                                                            current,
                                                            state::bap().sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize,
                                                            rosterAfter)) {
            current = rosterAfter;
            wrote = true;
        }
    }
    if (!wrote || framedSize == 0 || framedSize > response.size() || !queuez::valid(current)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=ability_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = current;
    // Clear the arm only after publication.
    session.abilityRefreshArmed = false;
    return true;
}

} // namespace

/** Publishes the next due reward, refresh, retry, or keepalive. */
bool consume_deferred(Session& session,
                      Scratch& scratch,
                      std::span<std::byte> response,
                      std::size_t& written,
                      bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated) {
        return false;
    }
    if (consume_account_resync(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // A failed resync blocks every incremental that could depend on its missing objects.
    if (session.accountResyncArmed) {
        return false;
    }
    WorldRewardRequest reward{};
    if (session.queuez.family4Active && bap::current_world_reward(reward)) {
        bool published = false;
        switch (reward.kind) {
        case WorldRewardKind::item:
            published = consume_world_item_acquisition(
                reward, session, scratch, response, written, touchesScratch);
            break;
        case WorldRewardKind::profileItem:
            published = consume_world_profile_item_acquisition(
                reward, session, scratch, response, written, touchesScratch);
            break;
        }
        if (published) {
            return true;
        }
    }
    if (consume_seasonal_experience_presentation(
            session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_ability_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (!session.family4RepushArmed || session.family4RepushRoot == 0
        || GetTickCount64() < session.family4RepushDueTick
        || GetTickCount64() < session.acquisitionPresentationUntilTick) {
        return consume_banner_repush(session, scratch, response, written, touchesScratch)
               || push::activity::consume_activity_keepalive(
                   session, scratch, response, written, touchesScratch);
    }
    // One attempt is owed, and it is spent whether or not it lands.
    touchesScratch = true;

    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kAccountFamilyType;
    subscription.familyRootSoid = session.family4RepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     after,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        // Neither failure clears on a retry. Holding the arm starves the keepalive, and the client
        // drops the activity session once the keepalive stops.
        session.family4RepushArmed = false;
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         framedSize == 0 ? "ev=queuez stage=repush result=fail reason=encode"
                                         : "ev=queuez stage=repush result=fail reason=capacity");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (queuez::valid(after)) {
        session.queuez = after;
    }
    session.family4RepushArmed = false;
    return true;
}

} // namespace sunrise::server::bap::encrypted
