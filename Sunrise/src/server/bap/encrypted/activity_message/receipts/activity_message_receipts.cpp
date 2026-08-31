/**
 * Framing handlers for every activity message that changes no State. Each one reads as much of its
 * body as the recovered grammar reaches, reports what it saw, and returns how completely the body
 * was read so the caller can record one arrival receipt. None of them acts on what it read.
 */

#include "activity_message_receipts.h"

#include "../../../internal.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/settings.h"
#include "../../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../../middleware/bap/activity_message/glimmer_extraction_contract.h"
#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../../middleware/bap/activity_message/start_activity.h"
#include "../../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../../middleware/encoding/byte_order.h"
#include "../../../../gameplay/mission/content_step_queue.h"
#include "../../../../gameplay/physics/host/physics_session.h"
#include "../../push/activity/activity_roster_research.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {
namespace {

namespace store = state::activity::receipts;
namespace authority = message::entity_authority;
namespace ledger = message::peer_ledger;
namespace telemetry = message::telemetry;

using store::Verdict;

/** Bounded research captures; each body is split into short log-safe rows. */
constexpr std::uint32_t kSenseProbeCaptureLimit = 64;
constexpr std::size_t kSenseProbeByteLimit = 1024;
constexpr std::size_t kSenseProbeChunk = 48;
/** Exact widths measured in the retained build-86657 raw Sense captures; semantics stay opaque. */
constexpr std::uint16_t kObjectiveDeltaBits = 76;
constexpr std::uint16_t kPlacementEngagementDeltaBits = 67;
constexpr std::uint16_t kEngagementSensorDeltaBits = 86;
/** Cross-build schema predictions; fail-closed until build-86657 bodies confirm them. */
constexpr std::array<std::uint16_t, 2> kHopOnCandidateDeltaBits{
    message::glimmer_extraction::kCrossBuildHopOnDescriptorBits,
    message::glimmer_extraction::kCrossBuildHopOnContainingBitsMax,
};
constexpr std::uint16_t kPublicEventSensorDeltaBits = 2;
std::atomic_uint32_t g_senseProbeCount{};
std::array<std::atomic_uint32_t, 3> g_glimmerShipGeneration{};

[[nodiscard]] std::uint64_t id_of(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) hash = (hash ^ byte) * 1099511628211ULL;
    return hash == 0 ? 1 : hash;
}

/**
 * Writes one bounded key-value event on the server channel.
 * @param level Severity, checked against the channel threshold before formatting.
 * @param format Printf-style format holding one event line.
 */
void report(core::log::Level level, const char* format, ...) noexcept {
    if (!core::log::accepts(core::log::Channel::server, level)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return;
    }
    // vsnprintf reports the length it wanted, so a truncated line reports past the buffer.
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::server, level, {line.data(), length});
}

/** @return The whole payload's bit count, which is the bar a fully framed body reaches. */
[[nodiscard]] std::size_t payload_bits(const message::Request& request) noexcept {
    return request.payload.size() * middleware::encoding::kBitsPerByte;
}

void report_sense_probe(const message::Request& request) noexcept {
    if (!core::settings::get().server.activation.trostlandSpawnerProbe) {
        return;
    }
    const std::uint32_t capture = g_senseProbeCount.fetch_add(1, std::memory_order_relaxed);
    if (capture >= kSenseProbeCaptureLimit) {
        return;
    }
    namespace sense = message::sense_update;
    sense::Envelope envelope{};
    std::size_t consumed = 0;
    const bool parsed = sense::parse_envelope(request.payload, envelope, consumed);
    report(parsed ? core::log::Level::info : core::log::Level::warn,
           "ev=activity_probe stage=sense_envelope result=%s capture=%u account=0x%016llX "
           "bytes=%zu consumed_bits=%zu global_delta=%u groups=%u",
           !parsed ? "malformed" : envelope.complete ? "framed" : "schema_required",
           capture,
           static_cast<unsigned long long>(request.accountHandle),
           request.payload.size(),
           consumed,
           static_cast<unsigned>(envelope.globalDeltaPresent),
           envelope.groupCount);
    if (parsed) {
        for (std::size_t index = 0; index < envelope.groupCount; ++index) {
            report(core::log::Level::info,
                   "ev=activity_probe stage=sense_group capture=%u index=%zu key=0x%08X bits=%u",
                   capture,
                   index,
                   envelope.groups[index].key,
                   envelope.groups[index].bits);
        }
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    const std::size_t captured =
        request.payload.size() < kSenseProbeByteLimit ? request.payload.size()
                                                      : kSenseProbeByteLimit;
    for (std::size_t offset = 0; offset < captured; offset += kSenseProbeChunk) {
        const std::size_t count =
            captured - offset < kSenseProbeChunk ? captured - offset : kSenseProbeChunk;
        std::array<char, kSenseProbeChunk * 2 + 1> hex{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = static_cast<unsigned char>(request.payload[offset + index]);
            hex[index * 2] = kHex[value >> 4U];
            hex[index * 2 + 1] = kHex[value & 0x0FU];
        }
        report(core::log::Level::info,
               "ev=activity_probe stage=sense_wire capture=%u offset=%zu bytes=%zu total=%zu hex=%s",
               capture,
               offset,
               count,
               request.payload.size(),
               hex.data());
    }
}

void report_fixed_glimmer_observation(const message::Request& request,
                                        const char* family,
                                        std::size_t site,
                                        std::uint8_t wireType,
                                        std::uint16_t slot,
                                        std::uint16_t deltaBits,
                                        std::uint32_t labelHash = 0) noexcept {
    message::sense_update::FixedObservation observation{};
    if (!message::sense_update::parse_fixed_observation(request.payload,
                                                        message::glimmer_extraction::kGroup,
                                                        wireType,
                                                        slot,
                                                        deltaBits,
                                                        observation)) {
        return;
    }
    // The build-87221 generic codec proves a 97-bit 32+32+32+1 descriptor. A
    // containing 98-bit capture has one extra bit whose position and source are unresolved.
    // Preserve only the raw delta; do not expose a field split without container alignment.
    const bool crossBuildHopOn = labelHash != 0
                                 && (deltaBits
                                         == message::glimmer_extraction::
                                                kCrossBuildHopOnDescriptorBits
                                     || deltaBits
                                            == message::glimmer_extraction::
                                                   kCrossBuildHopOnContainingBitsMax);
    report(core::log::Level::info,
           "ev=activity_probe stage=glimmer_observation result=logged family=%s site=%zu "
           "group=0x%08X wire_type=%u slot=%u delta_bits=%u label_hash=0x%08X "
           "xbuild_schema=%u xbuild_descriptor_bits=%u xbuild_packed_bytes=%u "
           "xbuild_alignment=%s delta_hash=0x%016llX "
           "delta_hi=0x%016llX delta_lo=0x%016llX generation=%u matches=%u",
           family,
           site,
           message::glimmer_extraction::kGroup,
           static_cast<unsigned>(wireType),
           slot,
           deltaBits,
           labelHash,
           crossBuildHopOn ? 87221U : 0U,
           crossBuildHopOn
               ? message::glimmer_extraction::kCrossBuildHopOnDescriptorBits
               : 0U,
           crossBuildHopOn ? message::glimmer_extraction::kCrossBuildHopOnPackedBytes : 0U,
           crossBuildHopOn ? "unresolved" : "na",
           static_cast<unsigned long long>(observation.deltaFingerprint),
           static_cast<unsigned long long>(observation.deltaHigh),
           static_cast<unsigned long long>(observation.deltaLow),
           observation.generation,
           observation.matches);

}



void report_bounded_glimmer_observation(const message::Request& request,
                                        const char* family,
                                        std::size_t site,
                                        std::uint8_t wireType,
                                        std::uint16_t slot) noexcept {
    message::sense_update::FixedObservation observation{};
    if (!message::sense_update::parse_single_bounded_observation(
            request.payload,
            message::glimmer_extraction::kGroup,
            wireType,
            slot,
            1,
            64,
            observation)) {
        return;
    }
    report(core::log::Level::info,
           "ev=activity_probe stage=glimmer_observation result=logged family=%s site=%zu "
           "group=0x%08X wire_type=%u slot=%u delta_bits=%u delta_hash=0x%016llX "
           "delta_hi=0x%016llX delta_lo=0x%016llX generation=%u matches=%u",
           family,
           site,
           message::glimmer_extraction::kGroup,
           static_cast<unsigned>(wireType),
           slot,
           observation.deltaBits,
           static_cast<unsigned long long>(observation.deltaFingerprint),
           static_cast<unsigned long long>(observation.deltaHigh),
           static_cast<unsigned long long>(observation.deltaLow),
           observation.generation,
           observation.matches);
}


/**
 * Reports one body whose declared framing did not hold.
 * @param stage Short stable stage name for the log line.
 * @param request Validated envelope.
 * @return Always malformed, so the caller can return it directly.
 */
[[nodiscard]] Verdict report_malformed(const char* stage,
                                       const message::Request& request) noexcept {
    report(core::log::Level::warn,
           "ev=activity stage=%s result=malformed type=%u bytes=%zu",
           stage,
           request.messageType,
           request.payload.size());
    return Verdict::malformed;
}

} // namespace

/** Frames a sensor sense update and reports its epoch. */
Framed frame_sense_update(const ActivityClientBinding& binding,
                          const message::Request& request) noexcept {
    namespace sense = message::sense_update;
    sense::SenseUpdate update{};
    std::size_t consumed = 0;
    if (!sense::parse_sense_update(request.payload, update, consumed)) {
        return {report_malformed("sense", request), consumed};
    }
    report_sense_probe(request);
    std::uint32_t spawnerGeneration = 0;
    std::uint32_t spawnerDelta = 0;
    if (sense::parse_trostland_spawner_generation(
            request.payload, spawnerGeneration, spawnerDelta)) {
        push::activity::observe_trostland_spawner_generation(spawnerGeneration, spawnerDelta);
        report(core::log::Level::info,
               "ev=spawner_probe stage=generation result=observed value=%u delta=0x%08X",
               spawnerGeneration,
               spawnerDelta);
    }
    std::uint32_t dropshipGeneration = 0;
    std::uint32_t dropshipDelta = 0;
    if (sense::parse_trostland_type1_generation(
            request.payload, 220, dropshipGeneration, dropshipDelta)
        || sense::parse_trostland_site0_spawn_generation(
            request.payload, dropshipGeneration, dropshipDelta)) {
        report(core::log::Level::info,
               "ev=mission stage=content_observation result=logged type=1 slot=220 generation=%u "
               "delta=0x%08X",
               dropshipGeneration,
               dropshipDelta);
    }
    namespace glimmer = message::glimmer_extraction;
    constexpr std::array<std::string_view, 3> kSpawnSignals{
        "site_1_ship_spawned", "site_2_ship_spawned", "site_3_ship_spawned"};
    constexpr std::array<std::string_view, 3> kSpawnSteps{
        "glimmer_site_1_ship_spawn", "glimmer_site_2_ship_spawn", "glimmer_site_3_ship_spawn"};
    for (std::size_t site = 0; site < glimmer::kSites.size(); ++site) {
        std::uint32_t generation = 0;
        std::uint32_t delta = 0;
        if (!sense::parse_type1_generation(request.payload,
                                           glimmer::kGroup,
                                           glimmer::kSites[site].dropship.slot,
                                           generation,
                                           delta)) continue;
        const std::uint32_t prior = g_glimmerShipGeneration[site].exchange(
            generation, std::memory_order_acq_rel);
        const bool fresh = generation != prior;
        gameplay::mission::ContentStepObservation observation{};
        const bool privateCurrent = binding.role == ActivityClientRole::privateCurrent
                                    && binding.session.sessionId == request.accountHandle;
        gameplay::physics::host::session::MissionContentSignalReservation signalReservation{};
        const bool reserved = privateCurrent
                              && gameplay::physics::host::session::reserve_mission_content_signal(
                                  request.accountHandle,
                                  binding.session.createdRevision,
                                  binding.bindingGeneration,
                                  id_of(kSpawnSignals[site]),
                                  signalReservation);
        struct ClaimContext final {
            std::uint64_t activitySessionId{};
            std::uint64_t hostGeneration{};
            std::uint64_t bindingGeneration{};
            std::uint64_t stepId{};
            std::uint32_t generation{};
            gameplay::mission::ContentStepObservation* observation{};
            bool claimed{};
        } claimContext{request.accountHandle,
                       binding.session.createdRevision,
                       binding.bindingGeneration,
                       id_of(kSpawnSteps[site]),
                       generation,
                       &observation,
                       false};
        const auto claim = [](void* opaque) noexcept {
            ClaimContext& context = *static_cast<ClaimContext*>(opaque);
            context.claimed = gameplay::mission::claim_content_step_observation(
                context.activitySessionId,
                context.hostGeneration,
                context.bindingGeneration,
                context.stepId,
                context.generation,
                *context.observation);
            return context.claimed;
        };
        const bool committed = reserved
                               && gameplay::physics::host::session::commit_mission_content_signal(
                                   signalReservation, claim, &claimContext);
        const bool claimed = claimContext.claimed;
        if (!committed) {
            gameplay::physics::host::session::abort_mission_content_signal(signalReservation);
        }
        const std::uint64_t queued = committed ? signalReservation.sequence : 0;
        report(core::log::Level::info,
               "ev=activity_probe stage=glimmer_ship result=%s site=%zu group=0x%08X "
               "type=1 slot=%u generation=%u prior=%u delta=0x%08X ticket=%llu request=%llu "
               "private=%u activity=0x%016llX revision=%llu binding=%llu",
               queued != 0 ? "queued"
                           : claimed ? "signal_refused"
                           : !privateCurrent ? "untrusted_role"
                           : !reserved ? "signal_backpressure"
                           : fresh ? "unclaimed" : "duplicate",
               site + 1,
               glimmer::kGroup,
               glimmer::kSites[site].dropship.slot,
               generation,
               prior,
               delta,
               static_cast<unsigned long long>(observation.ticket.value),
               static_cast<unsigned long long>(queued),
               privateCurrent ? 1U : 0U,
               static_cast<unsigned long long>(request.accountHandle),
               static_cast<unsigned long long>(binding.session.createdRevision),
               static_cast<unsigned long long>(binding.bindingGeneration));
    }
    for (std::size_t site = 0; site < glimmer::kSites.size(); ++site) {
        // These three parallel type-1 families are package-named normal defenders. Explicitly
        // named heroic wave spawners (slots 219-222) are intentionally not observed here.
        report_bounded_glimmer_observation(request,
                                           "normal_defenders",
                                           site + 1,
                                           2,
                                           glimmer::kSites[site].defenders.slot);
        report_bounded_glimmer_observation(request,
                                           "normal_defender_anchors",
                                           site + 1,
                                           2,
                                           glimmer::kDefenderAnchors[site]);
        report_bounded_glimmer_observation(request,
                                           "normal_defender_boss",
                                           site + 1,
                                           2,
                                           glimmer::kNormalBosses[site]);
        report_fixed_glimmer_observation(request,
                                         "defense_objective",
                                         site + 1,
                                         4,
                                         glimmer::kSites[site].objective.slot,
                                         kObjectiveDeltaBits);
        report_fixed_glimmer_observation(request,
                                         "placement_engagement",
                                         site + 1,
                                         31,
                                         glimmer::kPlacementEngagementMonitors[site].slot,
                                         kPlacementEngagementDeltaBits);
        for (const auto hopOnBits : kHopOnCandidateDeltaBits) {
            report_fixed_glimmer_observation(request,
                                             "normal_failure_hop_on",
                                             site + 1,
                                             27,
                                             glimmer::kFailureHopons[site].slot,
                                             hopOnBits,
                                             glimmer::kNormalFailureLabel);
            report_fixed_glimmer_observation(request,
                                             "normal_success_hop_on",
                                             site + 1,
                                             27,
                                             glimmer::kSuccessHopons[site].slot,
                                             hopOnBits,
                                             glimmer::kNormalSuccessLabel);
        }
    }
    report_fixed_glimmer_observation(request,
                                     "engagement_sensor",
                                     0,
                                     71,
                                     glimmer::kEngagementSensor,
                                     kEngagementSensorDeltaBits);
    report_fixed_glimmer_observation(request,
                                     "public_event_sensor",
                                     0,
                                     72,
                                     glimmer::kPublicEventSensor,
                                     kPublicEventSensorDeltaBits);
    report(core::log::Level::debug,
           "ev=activity stage=sense result=framed epoch=0x%016llX%016llX groups_bits=%u",
           static_cast<unsigned long long>(update.epoch.first),
           static_cast<unsigned long long>(update.epoch.second),
           update.tailBits);
    // The group loop behind the sense delta has no recovered width, so the body is retained
    // rather than walked.
    return {update.tailBits == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Records a service-8 envelope carrying the local-only activity-host request type. */
Framed frame_route_misuse(const message::Request& request) noexcept {
    // This type is a client-local message the transport turns into its own service. Arriving here
    // it is an authenticated but invalid route use, and answering it would allocate a second
    // session for one the client already has.
    report(core::log::Level::warn,
           "ev=activity stage=route result=misuse type=%u bytes=%zu",
           request.messageType,
           request.payload.size());
    return {Verdict::quarantined, 0};
}

/** Frames a start-new-activity request without applying any transition policy to it. */
Framed frame_start_activity(const message::Request& request) noexcept {
    namespace start = message::start_activity;
    start::StartActivity parsed{};
    std::size_t consumed = 0;
    if (!start::parse_start_activity(request.payload, parsed, consumed)) {
        return {report_malformed("start_activity", request), consumed};
    }
    report(core::log::Level::info,
           "ev=activity stage=start_activity result=read from=%d to=%d tail=%u",
           parsed.sourceActivityIndex,
           parsed.destinationActivityIndex,
           parsed.tailBits);
    return {parsed.tailBits == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Frames a peer-reservation request as far as its revision. */
Framed frame_reservation_request(const message::Request& request) noexcept {
    telemetry::ReservationRequest parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_reservation_request(request.payload, parsed, consumed)) {
        return {report_malformed("reservation", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=reservation result=read revision=%u records=%u",
           parsed.revision,
           parsed.recordBytes);
    const std::size_t tail =
        static_cast<std::size_t>(parsed.recordBytes) * middleware::encoding::kBitsPerByte;
    return {tail == 0 ? Verdict::framed : Verdict::partial, consumed};
}

/** Frames a reservation release. */
Framed frame_reservation_release(const message::Request& request) noexcept {
    ledger::ReservationRelease release{};
    std::size_t consumed = 0;
    if (!ledger::parse_release(request.payload, release, consumed)) {
        return {report_malformed("reservation_release", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=reservation_release result=read peer=0x%016llX",
           static_cast<unsigned long long>(release.peerKey));
    return {Verdict::framed, consumed};
}

/** Frames a peer leave notice. */
Framed frame_peer_leave(const message::Request& request) noexcept {
    ledger::PeerLeave leave{};
    std::size_t consumed = 0;
    if (!ledger::parse_leave(request.payload, leave, consumed)) {
        return {report_malformed("peer_leave", request), consumed};
    }
    report(core::log::Level::info,
           "ev=activity stage=peer_leave result=read peer=0x%016llX",
           static_cast<unsigned long long>(leave.peerKey));
    return {Verdict::framed, consumed};
}

/** Records a debug command without reading or running it. */
Framed frame_debug_command(const message::Request& request) noexcept {
    // The nested command definition is runtime selected, so the body cannot be walked from the
    // outer root alone. It is never executed, dispatched, or sent on to another client.
    report(core::log::Level::warn,
           "ev=activity stage=debug_command result=quarantined bytes=%zu",
           request.payload.size());
    return {Verdict::quarantined, 0};
}

/** Frames a connectivity failure report. */
Framed frame_connectivity_failure(const message::Request& request) noexcept {
    ledger::ConnectivityFailure failure{};
    std::size_t consumed = 0;
    if (!ledger::parse_connectivity_failure(request.payload, failure, consumed)) {
        return {report_malformed("connectivity", request), consumed};
    }
    report(core::log::Level::info,
           "ev=activity stage=connectivity result=read peer=0x%016llX reason=%u",
           static_cast<unsigned long long>(failure.peerKey),
           static_cast<unsigned>(failure.rawReason));
    // The two bits are the last schema field; the rest of the ninth byte is padding.
    return {Verdict::framed, consumed};
}

/** Records a client heartbeat as a bounded body. */
Framed frame_heartbeat(const message::Request& request) noexcept {
    // One runtime-selected nested definition, so the declared service length is the only bound.
    report(core::log::Level::debug,
           "ev=activity stage=heartbeat result=bounded bytes=%zu",
           request.payload.size());
    return {Verdict::partial, 0};
}

/** Frames a lag-switch report as far as its record count. */
Framed frame_lag_switch(const message::Request& request) noexcept {
    telemetry::LagSwitchReport parsed{};
    std::size_t consumed = 0;
    if (!telemetry::parse_lag_switch(request.payload, parsed, consumed)) {
        return {report_malformed("lag_switch", request), consumed};
    }
    report(parsed.aboveSupported ? core::log::Level::warn : core::log::Level::debug,
           "ev=activity stage=lag_switch result=%s records=%u tail=%u",
           parsed.aboveSupported ? "over_supported" : "read",
           static_cast<unsigned>(parsed.recordCount),
           parsed.recordBits);
    // A count above what the record grammar supports is retained and not acted on, because the
    // records behind it have no recovered shape either way.
    return {parsed.aboveSupported ? Verdict::quarantined : Verdict::partial, consumed};
}

/** Records a connection-quality report as a bounded body. */
Framed frame_connection_quality(const message::Request& request) noexcept {
    // Two nested structures whose leaf grammar is unresolved.
    report(core::log::Level::debug,
           "ev=activity stage=connection_quality result=bounded bytes=%zu",
           request.payload.size());
    return {Verdict::partial, 0};
}

/** Frames a speculative migration proposal without acting on it. */
Framed frame_migration(const message::Request& request) noexcept {
    ledger::MigrationProposal proposal{};
    std::size_t consumed = 0;
    if (!ledger::parse_migration(request.payload, proposal, consumed)) {
        return {report_malformed("migration", request), consumed};
    }
    // Host ownership never moves from a proposal. Acting on one needs the group migration state
    // machine, and a host that answers without it can split the session in two.
    report(core::log::Level::info,
           "ev=activity stage=migration result=noted peer=0x%016llX scalar=%d",
           static_cast<unsigned long long>(proposal.peerKey),
           proposal.scalar);
    return {Verdict::framed, consumed};
}

/** Frames the fixed high-water telemetry block. */
Framed frame_high_water(const message::Request& request) noexcept {
    telemetry::HighWater block{};
    std::size_t consumed = 0;
    if (!telemetry::parse_high_water(request.payload, block, consumed)) {
        return {report_malformed("high_water", request), consumed};
    }
    report(
        core::log::Level::debug, "ev=activity stage=high_water result=framed bits=%zu", consumed);
    return {Verdict::framed, consumed};
}

/** Frames one of the two opaque scalar messages. */
Framed frame_opaque_scalar(const message::Request& request) noexcept {
    std::int32_t value = 0;
    std::size_t consumed = 0;
    if (!telemetry::parse_opaque_scalar(request.payload, value, consumed)) {
        return {report_malformed("scalar", request), consumed};
    }
    report(core::log::Level::debug,
           "ev=activity stage=scalar result=read type=%u value=%d",
           request.messageType,
           value);
    return {Verdict::framed, consumed};
}

/** Frames the one-byte activity keepalive. */
Framed frame_client_keepalive(const message::Request& request) noexcept {
    namespace keepalive = message::client_keepalive;
    if (!keepalive::validate_client_keepalive(request.payload)) {
        return {report_malformed("keepalive", request), 0};
    }
    // The single byte is uninitialized at the sender, so it carries no value to read.
    const std::size_t consumed = payload_bits(request);
    return {Verdict::framed, consumed};
}

/** Frames one incident and quarantines a poison target. */
Framed frame_incident(const message::Request& request) noexcept {
    namespace incident = message::incident;
    incident::Incident parsed{};
    const incident::Verdict verdict = incident::validate(request.payload, parsed);
    const bool accepted = verdict == incident::Verdict::accepted;
    report(accepted ? core::log::Level::debug : core::log::Level::warn,
           "ev=activity stage=incident result=%s target=%u extra=%u selector=%u "
           "optional=%u payload=%u",
           incident::verdict_name(verdict),
           parsed.primaryTarget,
           parsed.extraTargetCount,
           parsed.selectorLength,
           static_cast<unsigned>(parsed.hasOptionalBlock),
           parsed.payloadLength);
    if (!accepted) {
        // A refused target index would index the consumer's table unbounded, so the body is kept
        // and never relayed.
        const Verdict outcome = verdict == incident::Verdict::targetPoisoned
                                        || verdict == incident::Verdict::targetOutOfRange
                                    ? Verdict::quarantined
                                    : Verdict::malformed;
        return {outcome, parsed.consumedBits};
    }
    return {Verdict::framed, parsed.consumedBits};
}

/** Frames one authority release, which records authority and returns no lease. */
Framed frame_authority_release(const message::Request& request, bool expectReason) noexcept {
    authority::Release decoded{};
    const bool parsed = expectReason ? authority::parse_abandon(request.payload, decoded)
                                     : authority::parse_abdicate(request.payload, decoded);
    if (!parsed) {
        return {report_malformed("authority", request), 0};
    }
    report(core::log::Level::debug,
           "ev=activity stage=authority result=noted type=%u selector=%u reason=%d",
           request.messageType,
           static_cast<unsigned>(decoded.selector),
           decoded.hasReason ? decoded.reason : 0);
    return {Verdict::framed, payload_bits(request)};
}

/** Frames one purge request. Nothing answers it. */
Framed frame_request_purge(const message::Request& request) noexcept {
    std::int32_t reason = 0;
    if (!authority::parse_request_purge(request.payload, reason)) {
        return {report_malformed("purge", request), 0};
    }
    // The answer would have to name the exact next authority generation, which nothing here
    // tracks, and the consumer asserts on any other value.
    report(core::log::Level::debug, "ev=activity stage=purge result=noted reason=%d", reason);
    return {Verdict::framed, payload_bits(request)};
}

/** Frames one authority query answer. */
Framed frame_query_answer(const message::Request& request) noexcept {
    authority::QueryAnswer answer{};
    if (!authority::parse_query_answer(request.messageType, request.payload, answer)) {
        return {report_malformed("authority_answer", request), 0};
    }
    // This host sends no query, so an answer is the client reconciling on its own.
    report(core::log::Level::debug,
           "ev=activity stage=authority result=answer type=%u corr=0x%08X selector=%d",
           request.messageType,
           answer.correlation,
           answer.hasSelector ? static_cast<int>(answer.selector) : -1);
    return {Verdict::framed, payload_bits(request)};
}

/** Records an envelope whose message type has no recovered body grammar. */
Framed frame_unknown(const message::Request& request) noexcept {
    report(core::log::Level::warn,
           "ev=activity stage=unknown result=bounded type=%u bytes=%zu",
           request.messageType,
           request.payload.size());
    return {Verdict::partial, 0};
}

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
