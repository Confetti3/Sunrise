#include "activity_roster_push.h"
#include "activity_roster_research.h"
#include "activity_entity_slot_republish.h"
#include "activity_roster_atomic.h"
#include "activity_message_push.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../client/hooks/squad_reference_probe/squad_reference_probe.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/bap/activity_message/glimmer_extraction_contract.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../gameplay/mission/enemy_wave_queue.h"
#include "../../../../gameplay/mission/content_step_queue.h"
#include "../../../../gameplay/physics/host/physics_session.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::sensor_auth_update;
namespace glimmer = middleware::bap::activity_message::glimmer_extraction;

[[nodiscard]] constexpr std::uint64_t mission_id(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) hash = (hash ^ byte) * 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] constexpr std::uint64_t enter_published_signal(
    gameplay::mission::ContentStepKind kind) noexcept {
    using Kind = gameplay::mission::ContentStepKind;
    switch (kind) {
    case Kind::glimmerSite0Enter: return mission_id("site_1_enter_published");
    case Kind::glimmerSite1Enter: return mission_id("site_2_enter_published");
    case Kind::glimmerSite2Enter: return mission_id("site_3_enter_published");
    default: return 0;
    }
}

void report_content_step(
    const Session& session,
    const message::Snapshot& snapshot,
    bool encoded,
    std::span<const std::byte> body,
    const gameplay::mission::QueuedContentStep* queued) noexcept {
    const message::ContentStep step = snapshot.contentStep.step;
    const std::uint32_t generation = snapshot.contentStep.generation;
    std::uint8_t ownerType = 0;
    std::uint16_t ownerSlot = 0;
    std::uint8_t targetType = 0;
    std::uint16_t targetSlot = 0;
    switch (step) {
    case message::ContentStep::glimmerSite0ShipSpawn:
        ownerType=1; ownerSlot=glimmer::kSites[0].dropship.slot;
        targetType=66; targetSlot=glimmer::kSites[0].dropshipRule.slot; break;
    case message::ContentStep::glimmerSite1ShipSpawn:
        ownerType=1; ownerSlot=glimmer::kSites[1].dropship.slot;
        targetType=66; targetSlot=glimmer::kSites[1].dropshipRule.slot; break;
    case message::ContentStep::glimmerSite2ShipSpawn:
        ownerType=1; ownerSlot=glimmer::kSites[2].dropship.slot;
        targetType=66; targetSlot=glimmer::kSites[2].dropshipRule.slot; break;
    case message::ContentStep::glimmerIntro:
        ownerType=5; ownerSlot=glimmer::kIntroSequence;
        targetType=58; targetSlot=glimmer::kSites[0].enterCommand.slot; break;
    case message::ContentStep::glimmerSite0Enter:
        ownerType=5; ownerSlot=glimmer::kActiveSequence;
        targetType=58; targetSlot=glimmer::kSites[0].enterCommand.slot; break;
    case message::ContentStep::glimmerSite0Exit:
        ownerType=5; ownerSlot=glimmer::kActiveSequence;
        targetType=58; targetSlot=glimmer::kSites[0].exitCommand.slot; break;
    case message::ContentStep::glimmerSite1Enter:
        ownerType=5; ownerSlot=glimmer::kActiveSequence;
        targetType=58; targetSlot=glimmer::kSites[1].enterCommand.slot; break;
    case message::ContentStep::glimmerSite1Exit:
        ownerType=5; ownerSlot=glimmer::kActiveSequence;
        targetType=58; targetSlot=glimmer::kSites[1].exitCommand.slot; break;
    case message::ContentStep::glimmerSite2Enter:
        ownerType=5; ownerSlot=glimmer::kActiveSequence;
        targetType=58; targetSlot=glimmer::kSites[2].enterCommand.slot; break;
    case message::ContentStep::glimmerSite2Exit:
        ownerType=5; ownerSlot=glimmer::kActiveSequence;
        targetType=58; targetSlot=glimmer::kSites[2].exitCommand.slot; break;
    default: break;
    }
    std::size_t groupRows = 0;
    std::size_t ownerMatches = 0;
    std::size_t ownerAuthMatches = 0;
    std::size_t targetMatches = 0;
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const message::Group& row = snapshot.roster.groups[group];
        if (row.key != glimmer::kGroup) continue;
        ++groupRows;
        for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
            if (row.slotTypes[slot] == ownerType && row.slotIndices[slot] == ownerSlot) {
                ++ownerMatches;
                if ((row.slotFlags[slot] & message::kSlotAuthFlag) != 0) {
                    ++ownerAuthMatches;
                }
            }
            if (row.slotTypes[slot] == targetType && row.slotIndices[slot] == targetSlot) {
                ++targetMatches;
            }
        }
    }
    const std::size_t authBits =
        message::auth_body_bits(snapshot, glimmer::kGroup, ownerType, ownerSlot, false);
    std::uint64_t bodyHash = body.empty() ? 0 : 14695981039346656037ULL;
    for (const std::byte value : body) {
        bodyHash = (bodyHash ^ std::to_integer<std::uint8_t>(value)) * 1099511628211ULL;
    }
    const std::uint64_t revision = queued != nullptr
                                       ? queued->hostGeneration
                                       : session.activity.session.createdRevision;
    const std::uint64_t binding = queued != nullptr
                                      ? queued->bindingGeneration
                                      : session.activity.bindingGeneration;
    const std::uint64_t ticket = queued != nullptr ? queued->ticket.value : 0;
    const std::uint64_t command = queued != nullptr ? queued->intent.commandId : 0;
    const std::uint64_t stepId = queued != nullptr ? queued->intent.stepId : 0;
    const unsigned kind = queued != nullptr
                              ? static_cast<unsigned>(queued->intent.kind)
                              : static_cast<unsigned>(gameplay::mission::ContentStepKind::count);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(),
        "ev=mission stage=content_step_wire result=%s activity=0x%016llX revision=%llu "
        "binding=%llu ticket=%llu command=%llu step_id=0x%016llX kind=%u wire_step=%u "
        "generation=%u group=0x%08X group_rows=%zu owner_type=%u owner_slot=%u "
        "owner_matches=%zu owner_auth=%zu target_type=%u target_slot=%u target_matches=%zu "
        "auth_bits=%zu body_bytes=%zu body_hash=0x%016llX",
        encoded ? "staged" : "encode",
        static_cast<unsigned long long>(session.activity.session.sessionId),
        static_cast<unsigned long long>(revision),
        static_cast<unsigned long long>(binding),
        static_cast<unsigned long long>(ticket),
        static_cast<unsigned long long>(command),
        static_cast<unsigned long long>(stepId), kind,
        static_cast<unsigned>(step), generation, glimmer::kGroup, groupRows,
        ownerType, ownerSlot, ownerMatches, ownerAuthMatches,
        targetType, targetSlot, targetMatches, authBits, body.size(),
        static_cast<unsigned long long>(bodyHash));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         encoded ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] message::ContentStep content_step(
    gameplay::mission::ContentStepKind kind) noexcept {
    using Mission = gameplay::mission::ContentStepKind;
    using Wire = message::ContentStep;
    switch (kind) {
    case Mission::glimmerIntro: return Wire::glimmerIntro;
    case Mission::glimmerSite0ShipSpawn: return Wire::glimmerSite0ShipSpawn;
    case Mission::glimmerSite0Enter: return Wire::glimmerSite0Enter;
    case Mission::glimmerSite0Exit: return Wire::glimmerSite0Exit;
    case Mission::glimmerSite1ShipSpawn: return Wire::glimmerSite1ShipSpawn;
    case Mission::glimmerSite1Enter: return Wire::glimmerSite1Enter;
    case Mission::glimmerSite1Exit: return Wire::glimmerSite1Exit;
    case Mission::glimmerSite2ShipSpawn: return Wire::glimmerSite2ShipSpawn;
    case Mission::glimmerSite2Enter: return Wire::glimmerSite2Enter;
    case Mission::glimmerSite2Exit: return Wire::glimmerSite2Exit;
    default: return Wire::none;
    }
}

/** No bubble was granted with this body. */
constexpr std::int32_t kNoGrant = -1;
/** The destination name a refusal reports. The selection field is 40 bytes wide. */
constexpr std::size_t kDestinationCapacity = 40;
constexpr std::uint32_t kRequestedMemberDelta = 0x220C3124;
std::atomic_uint32_t g_trostlandSpawnerGeneration{3};
std::atomic_uint32_t g_trostlandObservedDelta{};
std::atomic_uint32_t g_trostlandManualGeneration{};
std::atomic_uint32_t g_trostlandOutboundCaptureCount{};
std::atomic_uint64_t g_glimmerIntroToken{};
std::atomic_uint64_t g_glimmerIntroNext{1};
std::atomic_uint32_t g_glimmerIntroGeneration{1};

void report_trostland_outbound(std::span<const std::byte> body) noexcept {
    if (!core::settings::get().server.activation.trostlandSpawnerProbe) return;
    const std::uint32_t capture =
        g_trostlandOutboundCaptureCount.fetch_add(1, std::memory_order_relaxed);
    if (capture >= 8) return;
    constexpr char kHex[] = "0123456789ABCDEF";
    constexpr std::size_t kChunk = 48;
    for (std::size_t offset = 0; offset < body.size(); offset += kChunk) {
        const std::size_t count = (std::min)(kChunk, body.size() - offset);
        std::array<char, kChunk * 2 + 1> hex{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = static_cast<unsigned char>(body[offset + index]);
            hex[index * 2] = kHex[value >> 4U];
            hex[index * 2 + 1] = kHex[value & 0x0FU];
        }
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=spawner_probe stage=auth_wire capture=%u offset=%zu bytes=%zu total=%zu hex=%s",
            capture,
            offset,
            count,
            body.size(),
            hex.data());
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

} // namespace

void observe_trostland_spawner_generation(std::uint32_t generation,
                                          std::uint32_t delta) noexcept {
    g_trostlandSpawnerGeneration.store(generation, std::memory_order_release);
    if (delta != kRequestedMemberDelta) {
        g_trostlandObservedDelta.store(delta, std::memory_order_release);
    }
}

TrostlandSpawnerResearch trostland_spawner_research() noexcept {
    return {g_trostlandSpawnerGeneration.load(std::memory_order_acquire),
            g_trostlandObservedDelta.load(std::memory_order_acquire),
            g_trostlandManualGeneration.load(std::memory_order_acquire)};
}

void set_trostland_spawner_generation(std::uint32_t generation) noexcept {
    g_trostlandManualGeneration.store(generation, std::memory_order_release);
}

std::uint64_t request_glimmer_intro() noexcept {
    const std::uint64_t token = g_glimmerIntroNext.fetch_add(1, std::memory_order_acq_rel);
    if (token == 0) return 0;
    g_glimmerIntroToken.store(token, std::memory_order_release);
    return token;
}

bool peek_glimmer_intro(std::uint64_t& token, std::uint32_t& generation) noexcept {
    token = g_glimmerIntroToken.load(std::memory_order_acquire);
    generation = g_glimmerIntroGeneration.load(std::memory_order_acquire);
    return token != 0 && generation != 0;
}

void commit_glimmer_intro(std::uint64_t token) noexcept {
    if (token == 0) return;
    std::uint64_t expected = token;
    if (g_glimmerIntroToken.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        g_glimmerIntroGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
}

/** Appends one `sensor_auth_update` svc9 notification carrying the destination's roster. */
bool append_roster_body(Session& session,
                        Scratch& scratch,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool burst) noexcept {
    if (written > response.size() || session.activityRosterStaged.staged) {
        return false;
    }
    const std::uint32_t initialRosterGroups = session.activityRosterGroups;
    const std::uint8_t initialRosterSends = session.activityRosterSends;
    const std::uint8_t initialRosterState = session.activityRosterState;
    message::Snapshot snapshot{};
    std::array<char, kDestinationCapacity> destination{};
    std::size_t destinationLength = 0;
    RosterOutcome outcome = RosterOutcome::noEpoch;
    if (session.activityPatchEpoch.seen
        && session.activityPatchEpoch.bindingGeneration == session.activity.bindingGeneration) {
        outcome = build_roster_snapshot(
            session, scratch, snapshot, destination, destinationLength, burst);
    }
    const std::string_view name(destination.data(), destinationLength);
    if (outcome != RosterOutcome::published) {
        report_roster_push(session, snapshot, name, 0, kNoGrant, outcome);
        return false;
    }

    gameplay::mission::QueuedEnemyWave queued{};
    bool hasQueuedWave = session.activity.role == ActivityClientRole::privateCurrent
                         && gameplay::mission::peek_enemy_wave(
                             session.activity.session.sessionId,
                             session.activity.session.createdRevision,
                             session.activity.bindingGeneration,
                             queued);
    if (hasQueuedWave && queued.published) {
        const auto runtime =
            client::hooks::squad_reference_probe::runtime_snapshot();
        if (runtime.buildRequestCalls != 0 && runtime.requestedFirst == 1
            && runtime.requestedSecond == 0
            && gameplay::mission::settle_enemy_wave(queued.ticket)) {
            std::array<char, core::log::kLineCapacity> line{};
            const int settled = std::snprintf(
                line.data(),
                line.size(),
                "ev=mission stage=wave_settle result=confirmed activity=0x%016llX "
                "ticket=%llu requests=%llu",
                static_cast<unsigned long long>(session.activity.session.sessionId),
                static_cast<unsigned long long>(queued.ticket.value),
                static_cast<unsigned long long>(runtime.buildRequestCalls));
            if (settled > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(settled)});
            }
            hasQueuedWave = false;
        }
    }
    if (hasQueuedWave) {
        const gameplay::mission::EnemyWaveIntent& intent = queued.intent;
        if (intent.spawnerDefinition != 0x80C26B0A || intent.mode != 0
            || intent.requestedCount != 2 || intent.requested[0] != 1
            || intent.requested[1] != 0) {
            report_roster_push(session, snapshot, name, 0, kNoGrant, RosterOutcome::encodeFailed);
            return false;
        }
        snapshot.hasSense = true;
        snapshot.sense.group = 0x2986181D;
        snapshot.sense.definition = intent.spawnerDefinition;
        const std::uint32_t manualGeneration =
            g_trostlandManualGeneration.load(std::memory_order_acquire);
        const std::uint32_t observedGeneration =
            g_trostlandSpawnerGeneration.load(std::memory_order_acquire);
        snapshot.sense.generation = manualGeneration != 0
                                        ? manualGeneration
                                        : observedGeneration + 1;
        snapshot.sense.slotType = 1;
        snapshot.sense.slotIndex = 271;
        snapshot.sense.mode = intent.mode;
        snapshot.sense.requested = {intent.requested[0], intent.requested[1]};
        // Type 6 remains observation-only. Its 30-bit `0x80807ECC` delta is not the type-5 auth
        // body and must never be reflected into the client's sense mirror.
    }
    gameplay::mission::QueuedContentStep queuedContent{};
    bool hasQueuedContent = session.activity.role == ActivityClientRole::privateCurrent
                            && gameplay::mission::stage_content_step(
                                session.activity.session.sessionId,
                                session.activity.session.createdRevision,
                                session.activity.bindingGeneration,
                                queuedContent);
    if (hasQueuedContent && queuedContent.published) {
        const bool settled = gameplay::mission::settle_content_step(queuedContent.ticket);
        std::array<char, core::log::kLineCapacity> line{};
        const int logWritten = std::snprintf(
            line.data(), line.size(),
            "ev=mission stage=content_step_settle result=%s activity=0x%016llX "
            "revision=%llu binding=%llu ticket=%llu command=%llu step=0x%016llX kind=%u",
            settled ? "ok" : "fail",
            static_cast<unsigned long long>(queuedContent.activitySessionId),
            static_cast<unsigned long long>(queuedContent.hostGeneration),
            static_cast<unsigned long long>(queuedContent.bindingGeneration),
            static_cast<unsigned long long>(queuedContent.ticket.value),
            static_cast<unsigned long long>(queuedContent.intent.commandId),
            static_cast<unsigned long long>(queuedContent.intent.stepId),
            static_cast<unsigned>(queuedContent.intent.kind));
        if (logWritten > 0) {
            core::log::write(core::log::Channel::server,
                             settled ? core::log::Level::info : core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(logWritten)});
        }
        // A copied ticket that disappeared or changed lifetime must never be encoded after a
        // failed settlement. Either outcome removes this snapshot from the current body.
        hasQueuedContent = false;
    }
    gameplay::physics::host::session::MissionContentSignalReservation
        contentStepSignalReservation{};
    const std::uint64_t contentStepSignalId =
        hasQueuedContent ? enter_published_signal(queuedContent.intent.kind) : 0;
    if (contentStepSignalId != 0
        && !gameplay::physics::host::session::reserve_mission_content_signal(
            queuedContent.activitySessionId,
            queuedContent.hostGeneration,
            queuedContent.bindingGeneration,
            contentStepSignalId,
            contentStepSignalReservation)) {
        static_cast<void>(gameplay::mission::discard_staged_content_step(
            queuedContent.ticket));
        hasQueuedContent = false;
        std::array<char, core::log::kLineCapacity> line{};
        const int logWritten = std::snprintf(
            line.data(), line.size(),
            "ev=mission stage=enter_publication_signal result=backpressure "
            "activity=0x%016llX revision=%llu binding=%llu ticket=%llu signal=0x%016llX",
            static_cast<unsigned long long>(queuedContent.activitySessionId),
            static_cast<unsigned long long>(queuedContent.hostGeneration),
            static_cast<unsigned long long>(queuedContent.bindingGeneration),
            static_cast<unsigned long long>(queuedContent.ticket.value),
            static_cast<unsigned long long>(contentStepSignalId));
        if (logWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(logWritten)});
        }
    }
    const message::ContentStep queuedWireStep =
        hasQueuedContent ? content_step(queuedContent.intent.kind) : message::ContentStep::none;
    if (hasQueuedContent
        && (queuedWireStep == message::ContentStep::none
            || queuedContent.ticket.value > std::numeric_limits<std::uint32_t>::max())) {
        static_cast<void>(gameplay::mission::discard_staged_content_step(
            queuedContent.ticket));
        gameplay::physics::host::session::abort_mission_content_signal(
            contentStepSignalReservation);
        report_roster_push(session, snapshot, name, 0, kNoGrant, RosterOutcome::encodeFailed);
        return false;
    }
    std::uint64_t contentStepToken = 0;
    std::uint32_t contentStepGeneration = 0;
    const bool hasManualContent = !hasQueuedContent
                                  && session.activity.role == ActivityClientRole::privateCurrent
                                  && peek_glimmer_intro(contentStepToken,
                                                       contentStepGeneration);
    if (hasQueuedContent || hasManualContent) {
        snapshot.hasContentStep = true;
        snapshot.contentStep.step = hasQueuedContent ? queuedWireStep : message::ContentStep::glimmerIntro;
        snapshot.contentStep.generation = hasQueuedContent
                                              ? static_cast<std::uint32_t>(queuedContent.ticket.value)
                                              : contentStepGeneration;
    }

    // The grant is picked here and committed only once the frame reaches the caller, so a
    // discarded body leaves the bubble ungranted and the next push retries it.
    state::activity::bubble_authority::Grant grant{};
    // The grant follows the player, not the destination. The client names the region it is in
    // and that moves as it walks between bubbles, so granting the arrival bubble again would leave
    // the bubble the player actually entered without authority.
    // The wire field is unsigned. A value past the signed range turns negative and the selector
    // rejects it, the same answer as its own upper bound.
    if (state::activity::bubble_authority::select_grant(session.activity.session.sessionId,
                                                        static_cast<std::int32_t>(snapshot.region),
                                                        grant)) {
        snapshot.hasGrant = true;
        snapshot.grant.bubble = grant.bubble;
        snapshot.grant.token = grant.token;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    bool encoded = message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize)
                   && append_notification_frame(scratch,
                                                session.activity.session.sessionId,
                                                message::kMessageType,
                                                std::span(scratch.responseBody).first(messageSize),
                                                key,
                                                nonce,
                                                response,
                                                written);
    if (encoded && (snapshot.hasSense || snapshot.hasContentStep)) {
        report_trostland_outbound(std::span(scratch.responseBody).first(messageSize));
    }
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        // Staged, not published. The grant and the counters are one-way and this body may still be
        // discarded, so they are held here and settled by `commit_staged_roster` or
        // `discard_staged_roster`.
        session.activityRosterStaged.grant = grant;
        session.activityRosterStaged.bindingGeneration = session.activity.bindingGeneration;
        session.activityRosterStaged.priorGroups = initialRosterGroups;
        session.activityRosterStaged.priorSends = initialRosterSends;
        session.activityRosterStaged.priorState = initialRosterState;
        session.activityRosterStaged.hasGrant = snapshot.hasGrant;
        session.activityRosterStaged.enemyWaveTicket = queued.ticket.value;
        session.activityRosterStaged.hasEnemyWave = hasQueuedWave;
        session.activityRosterStaged.contentStepToken = contentStepToken;
        session.activityRosterStaged.contentStepTicket = queuedContent.ticket.value;
        session.activityRosterStaged.contentStepSignalReservation =
            contentStepSignalReservation.sequence;
        session.activityRosterStaged.hasContentStep = hasManualContent;
        session.activityRosterStaged.hasQueuedContentStep = hasQueuedContent;
        session.activityRosterStaged.hasContentStepSignalReservation =
            contentStepSignalReservation.sequence != 0;
        session.activityRosterStaged.staged = true;
    }
    constexpr std::uint8_t kEncodeFailureReason =
        static_cast<std::uint8_t>(RosterOutcome::encodeFailed) + 1U;
    if (snapshot.hasContentStep
        && (encoded || session.activityRosterReason != kEncodeFailureReason)) {
        const std::span<const std::byte> contentBody =
            encoded ? std::span<const std::byte>(scratch.responseBody.data(), messageSize)
                    : std::span<const std::byte>{};
        report_content_step(session,
                            snapshot,
                            encoded,
                            contentBody,
                            hasQueuedContent ? &queuedContent : nullptr);
    }
    report_roster_push(session,
                       snapshot,
                       name,
                       encoded ? messageSize : 0,
                       encoded && snapshot.hasGrant ? snapshot.grant.bubble : kNoGrant,
                       encoded ? RosterOutcome::published : RosterOutcome::encodeFailed);
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    if (!encoded) {
        if (hasQueuedContent) {
            static_cast<void>(gameplay::mission::discard_staged_content_step(
                queuedContent.ticket));
        }
        gameplay::physics::host::session::abort_mission_content_signal(
            contentStepSignalReservation);
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
        session.activityRosterGroups = initialRosterGroups;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

/** Appends a manual authentic type-0 republish and the roster as one byte/nonce unit. */
bool append_roster_notification(Session& session,
                                Scratch& scratch,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool burst) noexcept {
    if (written > response.size() || session.activityRosterStaged.staged) return false;
    entity_slot_republish::Selection republish{};
    if (!entity_slot_republish::select(session, republish)) {
        return append_roster_body(
            session, scratch, key, nonce, response, written, burst);
    }

    const bool encoded = detail::append_entity_slot_roster_pair(
        nonce,
        response,
        written,
        [&]() noexcept {
            return append_entity_slot_notification(scratch,
                                                   session.activity.session.sessionId,
                                                   republish.held,
                                                   key,
                                                   nonce,
                                                   response,
                                                   written);
        },
        [&]() noexcept {
            return append_roster_body(
                session, scratch, key, nonce, response, written, burst);
        },
        [&]() noexcept { discard_staged_roster(session); });
    if (!encoded || !session.activityRosterStaged.staged) {
        entity_slot_republish::mark_encode_failed(republish.token);
        return false;
    }

    entity_slot_republish::stage_publication(session, republish);
    return true;
}

/** Counts one delivered manual type-0 token before any delivered rebind is published. */
void commit_staged_entity_slot_republish(Session& session) noexcept {
    entity_slot_republish::commit_staged_publication(session);
}

/** Settles a staged roster body that reached the caller. */
void commit_staged_roster(Session& session) noexcept {
    if (!session.activityRosterStaged.staged) {
        return;
    }
    // Delivery bookkeeping is not binding-scoped. Keep this fallback for callers with no rebind
    // publication step; transaction callers invoke it directly after their caller copy.
    commit_staged_entity_slot_republish(session);
    // The caller already received this body. Its queue-owned pin is therefore committed before
    // consulting mutable connection fields; a concurrent rebind cannot turn delivery into discard.
    if (session.activityRosterStaged.hasQueuedContentStep) {
        gameplay::mission::QueuedContentStep committedContent{};
        struct CommitContext final {
            gameplay::mission::ContentStepTicket ticket{};
            gameplay::mission::QueuedContentStep* output{};
            bool invoked{};
            bool marked{};
        } context{{session.activityRosterStaged.contentStepTicket}, &committedContent, false, false};
        const auto commitTicket = [](void* opaque) noexcept {
            CommitContext& value = *static_cast<CommitContext*>(opaque);
            value.invoked = true;
            value.marked = gameplay::mission::commit_staged_content_step(
                value.ticket, *value.output);
            return value.marked;
        };
        bool signalCommitted = false;
        if (session.activityRosterStaged.hasContentStepSignalReservation) {
            signalCommitted = gameplay::physics::host::session::commit_mission_content_signal(
                {session.activityRosterStaged.contentStepSignalReservation},
                commitTicket,
                &context);
            if (!signalCommitted) {
                gameplay::physics::host::session::abort_mission_content_signal(
                    {session.activityRosterStaged.contentStepSignalReservation});
            }
        }
        if (!context.invoked) {
            static_cast<void>(commitTicket(&context));
        }
        const bool marked = context.marked;
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(), line.size(),
            "ev=mission stage=content_step_commit result=%s activity=0x%016llX "
            "revision=%llu binding=%llu ticket=%llu queue_match=%u command=%llu "
            "step=0x%016llX kind=%u enter_signal=%s signal_ticket=%llu",
            marked ? "ok" : "fail",
            static_cast<unsigned long long>(marked ? committedContent.activitySessionId : 0),
            static_cast<unsigned long long>(marked ? committedContent.hostGeneration : 0),
            static_cast<unsigned long long>(marked ? committedContent.bindingGeneration : 0),
            static_cast<unsigned long long>(session.activityRosterStaged.contentStepTicket),
            marked ? 1U : 0U,
            static_cast<unsigned long long>(marked ? committedContent.intent.commandId : 0),
            static_cast<unsigned long long>(marked ? committedContent.intent.stepId : 0),
            marked ? static_cast<unsigned>(committedContent.intent.kind)
                   : static_cast<unsigned>(gameplay::mission::ContentStepKind::count),
            !session.activityRosterStaged.hasContentStepSignalReservation ? "none"
                : signalCommitted ? "queued"
                : marked ? "scope_retired" : "ticket_failed",
            static_cast<unsigned long long>(
                session.activityRosterStaged.contentStepSignalReservation));
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             marked && (!session.activityRosterStaged.hasContentStepSignalReservation
                                        || signalCommitted)
                                 ? core::log::Level::info : core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (session.activityRosterStaged.bindingGeneration != session.activity.bindingGeneration) {
        session.activityRosterStaged = {};
        return;
    }
    if (session.activityRosterStaged.hasGrant) {
        state::activity::bubble_authority::record_grant(session.activity.session.sessionId,
                                                        session.activityRosterStaged.grant);
    }
    if (session.activityRosterStaged.hasEnemyWave) {
        static_cast<void>(gameplay::mission::mark_enemy_wave_published(
            {session.activityRosterStaged.enemyWaveTicket}));
    }
    if (session.activityRosterStaged.hasContentStep) {
        commit_glimmer_intro(session.activityRosterStaged.contentStepToken);
    }
    session.activityRosterStaged = {};
}

/** Puts back what a staged roster body advanced, now that the body has been discarded. */
void discard_staged_roster(Session& session) noexcept {
    if (!session.activityRosterStaged.staged) {
        return;
    }
    entity_slot_republish::discard_staged_publication(session);
    if (session.activityRosterStaged.hasContentStepSignalReservation) {
        gameplay::physics::host::session::abort_mission_content_signal(
            {session.activityRosterStaged.contentStepSignalReservation});
    }
    if (session.activityRosterStaged.hasQueuedContentStep) {
        static_cast<void>(gameplay::mission::discard_staged_content_step(
            {session.activityRosterStaged.contentStepTicket}));
    }
    // The client never saw this body, so its state byte must not be spent. The next push has to
    // move the byte again or the client does not rebuild its roster objects.
    session.activityRosterGroups = session.activityRosterStaged.priorGroups;
    session.activityRosterSends = session.activityRosterStaged.priorSends;
    session.activityRosterState = session.activityRosterStaged.priorState;
    session.activityRosterStaged = {};
}

} // namespace sunrise::server::bap::encrypted::push::activity
