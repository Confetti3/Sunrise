#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/account_translation/account_translation_response.h"
#include "../../../../middleware/bap/activity_host/activity_host_response.h"
#include "../../../../middleware/bap/certificate.h"
#include "../../../../middleware/bap/client_config/client_config_response.h"
#include "../../../../middleware/bap/family_subscription.h"
#include "../../../../middleware/bap/family_unsubscription.h"
#include "../../../../middleware/bap/user_message/user_message_response.h"
#include "../../../../middleware/encoding/byte_order.h"
#include "../../../../middleware/web_service/messages/opcode505/opcode505_codec.h"
#include "../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../gameplay/group/group_host_sessions.h"
#include "../../../web_service/web_service_runtime.h"
#include "../activity_host_manager/activity_host_manager_route.h"
#include "../activity_message/activity_message_route.h"
#include "../internal.h"
#include "../matchmaking/matchmaking_route.h"
#include "../queuez/queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::body {
namespace {

/** One line carries the family and the root soid and nothing else. */
constexpr std::size_t kSubscribeReportLimit = 96;
/**
 * Identity already paired with the account soid, or zero before the first pairing.
 * There is one account, so this is process-wide rather than per connection.
 */
std::atomic<std::uint64_t> g_translatedIdentity{0};

/**
 * Reports whether one svc-23 request may be paired with the account soid.
 * The reply writes the soid into a queuez roster member. Two identities on one soid put two
 * family-zero source entries on it, and every lookup then resolves only the first.
 * @param identity Validated nonzero svc-23 identity.
 * @return True when this identity is the one paired, or the first to ask.
 */
[[nodiscard]] bool pairs_identity(std::uint64_t identity) noexcept {
    std::uint64_t claimed = 0;
    // A repeat of the same identity still pairs: the peer re-asks until the flag sticks.
    return g_translatedIdentity.compare_exchange_strong(
               claimed, identity, std::memory_order_relaxed)
           || claimed == identity;
}

} // namespace

/**
 * Processes the body for one authenticated service route.
 * @param route Service route data found earlier.
 * @param queuezState Queuez versions and residents set up by this BAP peer.
 * @param activity Exact ActivityClient generation owned by this BAP session.
 * @param rosterDecode Last complete msg-5 identity map delivered on this connection.
 * @param matchmakingContext State-owned logical context for this BAP session.
 * @param requestBody Borrowed decrypted request body.
 * @param output Caller-owned response-body storage.
 * @param written Receives encoded body bytes.
 * @param outcome Receives one validated transport action or deferred State transaction.
 * @return True when the chosen body codec succeeds.
 */
bool process(const ServiceRoute& route,
             const queuez::SessionState& queuezState,
             const ActivityClientBinding& activity,
             const RosterDecodeMap& rosterDecode,
             state::matchmaking::ContextHandle matchmakingContext,
             std::span<const std::byte> requestBody,
             std::span<std::byte> output,
             std::size_t& written,
             ServiceOutcome& outcome) noexcept {
    outcome = {};
    switch (route.bodyCodec) {
    case BodyCodec::empty:
        written = 0;
        return true;
    case BodyCodec::accountTranslationResponse: {
        const state::AccountState account = state::account_snapshot();
        std::uint64_t identity = 0;
        server::gameplay::group::HostSessionBinding host{};
        const bool parsed =
            middleware::bap::account_translation::request_identity(requestBody, identity);
        // A host peer has no platform handle, so the client asks by identity zero on the primary
        // link. The answer is the live region session id, a non-zero soid the peer watcher accepts.
        // A remote msg-12 row is asked by activity-session id and answers with the row's soid.
        const std::uint64_t liveSession =
            state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
        const bool zeroHandle =
            parsed && identity == 0 && liveSession != state::activity::kAbsentSessionId;
        const bool logicalHost =
            parsed && identity != 0
            && server::gameplay::group::host_session_for_activity(identity, host)
            && host.target.sessionId == identity;
        const bool accountIdentity =
            parsed && identity != 0 && !logicalHost && pairs_identity(identity);
        // A zero SOID writes the valid zero-entry reply, so an unknown identity never stalls.
        const std::uint64_t soid = zeroHandle        ? liveSession
                                   : logicalHost     ? host.target.sessionId
                                   : accountIdentity ? account.primarySoid
                                                     : 0;
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=queuez stage=translate result=%s identity=0x%016llX "
                                        "soid=0x%016llX",
                                        zeroHandle        ? "paired kind=zero_handle"
                                        : logicalHost     ? "paired kind=logical_host"
                                        : accountIdentity ? "paired kind=account"
                                                          : "unpaired",
                                        static_cast<unsigned long long>(identity),
                                        static_cast<unsigned long long>(soid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return middleware::bap::account_translation::encode_response(
            requestBody, soid, output, written);
    }
    case BodyCodec::activityHostManagerResponse: {
        state::activity::PendingAllocation allocation{};
        bool hasAllocation = false;
        const bool encoded = activity_host_manager::encode_response(
            requestBody, output, written, allocation, hasAllocation);
        if (encoded && hasAllocation) {
            outcome.transaction = allocation;
        }
        return encoded;
    }
    case BodyCodec::activityMessageRequest: {
        written = 0;
        activity_message::ActivityPlan plan{};
        bool hasTransaction = false;
        const bool processed =
            activity_message::process(activity, rosterDecode, requestBody, plan, hasTransaction);
        if (processed && hasTransaction) {
            outcome.transaction = plan;
        }
        return processed;
    }
    case BodyCodec::activityHostResponse: {
        const state::SignOnState& signOn = state::sign_on();
        return middleware::bap::activity_host::encode_response(
            requestBody, signOn.relayAddress, signOn.relayPort, output, written);
    }
    case BodyCodec::clientConfigResponse:
        return middleware::bap::client_config::encode_minimal_response(output, written);
    case BodyCodec::familySubscription: {
        written = 0;
        outcome.hasSubscription =
            middleware::bap::family_subscription::parse(requestBody, outcome.subscription);
        // The subscribe names the record now ready for a snapshot. The family and root are the
        // only way to tell one record's cycle from several records interleaving.
        std::array<char, kSubscribeReportLimit> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=queuez stage=subscribe result=%s family=%u root=0x%016llX",
                          outcome.hasSubscription ? "ok" : "unreadable",
                          static_cast<unsigned>(outcome.subscription.familyType),
                          static_cast<unsigned long long>(outcome.subscription.familyRootSoid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return outcome.hasSubscription;
    }
    case BodyCodec::familyUnsubscription: {
        written = 0;
        outcome.hasUnsubscription =
            middleware::bap::family_unsubscription::parse(requestBody, outcome.unsubscription);
        return outcome.hasUnsubscription;
    }
    case BodyCodec::matchmakingResponse: {
        state::matchmaking::PendingMutation mutation{};
        bool hasMutation = false;
        state::PendingCurrentActivity currentActivity{};
        const bool encoded = matchmaking::encode_response(matchmakingContext,
                                                          requestBody,
                                                          output,
                                                          written,
                                                          mutation,
                                                          hasMutation,
                                                          currentActivity);
        if (encoded && hasMutation) {
            outcome.transaction = mutation;
        } else if (encoded && currentActivity.prepared) {
            // The reply completes the client's task either way. A character update that cannot
            // be staged is dropped, not turned into a missing reply.
            auto& transaction = outcome.transaction.emplace<CurrentActivityTransaction>();
            if (!queuez::stage_current_activity_character(
                    queuezState, currentActivity.characterSoid, transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=current_activity stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
            } else {
                transaction.pending = currentActivity;
            }
        }
        return encoded;
    }
    case BodyCodec::steamCertificate:
        return middleware::bap::certificate::encode_response(requestBody, output, written);
    case BodyCodec::userMessageResponse:
        return middleware::bap::user_message::encode_minimal_response(output, written);
    case BodyCodec::webService: {
        middleware::web_service::Message message;
        if (middleware::web_service::parse_request(requestBody, message)
            && message.opcode == middleware::web_service::messages::opcode505::kOpcode) {
            if (!middleware::web_service::messages::opcode505::parse_request(message)
                || !queuez::stage_change_character(queuezState, outcome.changeCharacter)
                || !middleware::web_service::messages::opcode505::encode_response(
                    message, outcome.changeCharacter.after.family4Version, output, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws505 stage=change result=fail");
                // The plain status pair still goes out. The Client's Change Character waits on the
                // echoed transaction id, so a missing reply hangs it for the rest of the run. It
                // reports the refusal, because no revision publishes the change.
                outcome.changeCharacter = {};
                middleware::web_service::StatusResponse refusal{};
                refusal.code = middleware::web_service::kRefusedStatusCode;
                refusal.value = middleware::web_service::kNoFamily4Publication;
                return middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    refusal,
                    output,
                    written);
            }
            outcome.hasChangeCharacter = true;
            return true;
        }
        web_service::Outcome webOutcome;
        if (!sunrise::server::web_service::consume(requestBody, output, written, webOutcome)) {
            return false;
        }
        outcome.hasSubscription = webOutcome.hasSubscription;
        outcome.subscription = webOutcome.subscription;
        const auto* equipmentSwap =
            web_service::mutation_if<state::PendingEquipmentSwap>(webOutcome);
        const auto* subclassSelection =
            web_service::mutation_if<state::PendingSubclassSelection>(webOutcome);
        const auto* socketPlug = web_service::mutation_if<state::PendingSocketPlug>(webOutcome);
        const auto* itemState = web_service::mutation_if<state::PendingItemState>(webOutcome);
        const auto* itemAcquisition =
            web_service::mutation_if<state::PendingItemAcquisition>(webOutcome);
        const auto* profileItemAcquisition =
            web_service::mutation_if<state::PendingProfileItemAcquisition>(webOutcome);
        const auto* itemDismantle =
            web_service::mutation_if<state::PendingItemDismantle>(webOutcome);
        if (equipmentSwap != nullptr) {
            // Equip is an optimistic Character-screen action. Its status-pair value is the exact
            // Family-4 revision whose following Queuez frame makes it authoritative. Stage that
            // revision before encoding the reply, or the Client completes against the old store.
            auto& transaction = outcome.transaction.emplace<EquipmentSwapTransaction>();
            if (!queuez::stage_equipment_swap(
                    queuezState, equipmentSwap->characterSoid, transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws403 stage=queuez_preflight result=fail");
                // The reply is already encoded as a success. Nothing moves now, so it is rewritten
                // as a refusal rather than dropped, which would hang the correlated task.
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=ws403 stage=response result=fail");
                    return false;
                }
                web_service::report_equip_response(message, status.value, output.first(written));
                transaction.pending = *equipmentSwap;
            }
        }
        if (subclassSelection != nullptr) {
            // Opcode 801 completes at the exact Family-4 revision carrying the selected subclass
            // socket entry. The resident manifest and equipped subclass identity stay unchanged.
            auto& transaction = outcome.transaction.emplace<SubclassSelectionTransaction>();
            if (!queuez::stage_subclass_selection(queuezState,
                                                  subclassSelection->accountSoid,
                                                  subclassSelection->characterSoid,
                                                  subclassSelection->subclassInstanceSoid,
                                                  transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=subclass_select stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=subclass_select stage=response result=fail");
                    return false;
                }
                web_service::report_subclass_selection_response(
                    message, status.value, *subclassSelection, output.first(written));
                transaction.pending = *subclassSelection;
            }
        }
        if (socketPlug != nullptr) {
            // Opcode 903 completes at the exact Family-4 revision carrying the changed resident
            // item instance. The resident manifest and character placement remain unchanged.
            auto& transaction = outcome.transaction.emplace<SocketPlugTransaction>();
            if (!queuez::stage_socket_plug(queuezState,
                                           socketPlug->accountSoid,
                                           socketPlug->characterSoid,
                                           socketPlug->targetInstanceSoid,
                                           socketPlug->profileChanged,
                                           transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=socket_plug stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=socket_plug stage=response result=fail");
                    return false;
                }
                web_service::report_socket_plug_response(message,
                                                         status.value,
                                                         socketPlug->targetInstanceSoid,
                                                         socketPlug->socketLane,
                                                         socketPlug->plugDefinitionIndex,
                                                         output.first(written));
                transaction.pending = *socketPlug;
            }
        }
        if (itemState != nullptr) {
            // Opcode 406 completes at the exact Family-4 revision carrying the changed inventory
            // row flags. Placement and every resident item-instance body remain unchanged.
            auto& transaction = outcome.transaction.emplace<ItemStateTransaction>();
            if (!queuez::stage_equipment_swap(
                    queuezState, itemState->characterSoid, transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=item_state stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=item_state stage=response result=fail");
                    return false;
                }
                transaction.pending = *itemState;
            }
        }
        if (itemAcquisition != nullptr) {
            // A Collections pull is complete only at the exact Family-4 revision that adds both
            // the inventory row and its newly resident instance object. Stage that revision before
            // re-encoding the correlated status pair, just like an equipment swap.
            auto& transaction = outcome.transaction.emplace<ItemAcquisitionTransaction>();
            if (!queuez::stage_item_acquisition(queuezState,
                                                itemAcquisition->accountSoid,
                                                itemAcquisition->characterSoid,
                                                itemAcquisition->acquiredInstanceSoid,
                                                itemAcquisition->profileChanged,
                                                transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=acquire stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=acquire stage=response result=fail");
                    return false;
                }
                web_service::report_item_acquisition_response(message,
                                                              status.value,
                                                              itemAcquisition->acquiredInstanceSoid,
                                                              output.first(written));
                transaction.pending = *itemAcquisition;
            }
        }
        if (profileItemAcquisition != nullptr) {
            // Profile stacks live in the account body. Actionable shaders/modifications also name
            // a Family-4 item resident: an existing stack must already own it, while a newly
            // appended row adds it atomically at this exact +1 revision.
            auto& transaction = outcome.transaction.emplace<ProfileItemAcquisitionTransaction>();
            if (!queuez::stage_profile_item_acquisition(
                    queuezState,
                    profileItemAcquisition->accountSoid,
                    profileItemAcquisition->acquiredInstanceSoid,
                    profileItemAcquisition->actionSource,
                    profileItemAcquisition->appended,
                    transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=profile_acquire stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=profile_acquire stage=response result=fail");
                    return false;
                }
                web_service::report_profile_item_acquisition_response(
                    message,
                    status.value,
                    profileItemAcquisition->acquiredDefinitionHash,
                    profileItemAcquisition->acquiredQuantity,
                    output.first(written));
                transaction.pending = *profileItemAcquisition;
            }
        }
        if (itemDismantle != nullptr) {
            // Dismantle is another optimistic Character-screen action. Promise only the exact
            // Family-4 revision carrying both the character after-image and the empty release
            // descriptor; otherwise keep the generic sentinel reply and publish no removal.
            auto& transaction = outcome.transaction.emplace<ItemDismantleTransaction>();
            if (!queuez::stage_item_dismantle(queuezState,
                                              itemDismantle->accountSoid,
                                              itemDismantle->characterSoid,
                                              itemDismantle->dismantledInstanceSoid,
                                              itemDismantle->profileChanged,
                                              transaction.update)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=dismantle stage=queuez_preflight result=fail");
                outcome.transaction = std::monostate{};
                (void)web_service::encode_staging_refusal(message, output, written);
            } else {
                middleware::web_service::StatusResponse status{};
                status.value = transaction.update.after.family4Version;
                if (!middleware::web_service::encode_response(
                        message,
                        middleware::web_service::ResponseShape::statusPair,
                        status,
                        output,
                        written)) {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::warn,
                                     "ev=dismantle stage=response result=fail");
                    return false;
                }
                web_service::report_item_dismantle_response(message,
                                                            status.value,
                                                            itemDismantle->dismantledInstanceSoid,
                                                            output.first(written));
                transaction.pending = *itemDismantle;
            }
        }
        // A pick that names the resident character moves nothing, so staging refuses it and the
        // reply still stands on its own.
        if (webOutcome.hasSelectedCharacter
            && queuez::stage_select_character(
                queuezState, webOutcome.selectedCharacterSoid, outcome.selectCharacter)) {
            outcome.hasSelectCharacter = true;
            // Same rule as every other mutating opcode: the reply names the revision whose next
            // queuez frame makes the pick authoritative. Without it the Client completes the pick
            // against the store it already had.
            middleware::web_service::StatusResponse status{};
            status.value = outcome.selectCharacter.after.family4Version;
            if (!middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    status,
                    output,
                    written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws504 stage=response result=fail");
                return false;
            }
        } else {
            outcome.selectCharacter = {};
        }
        return true;
    }
    }
    written = 0;
    return false;
}

} // namespace sunrise::server::bap::encrypted::body
