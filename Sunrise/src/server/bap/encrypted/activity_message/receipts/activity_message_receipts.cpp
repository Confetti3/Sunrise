/**
 * Framing handlers for activity messages. Each reads as much of its body as the recovered grammar
 * reaches and returns how completely it was read so the caller can record one arrival receipt.
 * Pickup incidents also grant their resolved lore record and request a fresh account image.
 */

#include "../../../../../client/player/player_position.h"
#include "../../../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/build_data/sobjects/sobject_catalog.h"
#include "../../../../../state/lore/lore_grant.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../bap/internal.h"
#include "activity_message_receipts.h"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/activity_client_keepalive_validator.h"
#include "../../../../../middleware/bap/activity_message/entity_authority.h"
#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../../middleware/bap/activity_message/sense_update.h"
#include "../../../../../middleware/bap/activity_message/start_activity.h"
#include "../../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../../middleware/crypto/random_bytes.h"
#include "../../../../../middleware/encoding/byte_order.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {
namespace {

namespace store = state::activity::receipts;
namespace authority = message::entity_authority;
namespace ledger = message::peer_ledger;
namespace telemetry = message::telemetry;

using store::Verdict;

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

struct EggResolution {
    state::lore::GrantOutcome outcome{state::lore::GrantOutcome::recordNotFound};
    std::uint16_t record{};
    bool resolved{};
};

struct EggLootResolution {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    bool granted{};
};

/** Grants one installed Dreaming City weapon or active-class Reverie Dawn armour piece. */
[[nodiscard]] EggLootResolution grant_random_egg_loot() noexcept {
    constexpr std::array<std::uint32_t, 7> kWeapons{
        640114618U, 334171687U, 346136302U, 3242168339U,
        3297863558U, 3740842661U, 1644162710U,
    };
    constexpr std::array<std::uint32_t, 5> kTitanArmour{
        1472713738U, 1478378067U, 2561756285U, 4257800469U, 4023744176U,
    };
    constexpr std::array<std::uint32_t, 5> kHunterArmour{
        2804026582U, 4008120231U, 2467635521U, 3185383401U, 844097260U,
    };
    constexpr std::array<std::uint32_t, 5> kWarlockArmour{
        1076538039U, 150052158U, 757360370U, 569434520U, 1394177923U,
    };

    std::array<std::uint32_t, kWeapons.size() + kTitanArmour.size()> hashes{};
    std::size_t hashCount = 0;
    for (const std::uint32_t hash : kWeapons) {
        hashes[hashCount++] = hash;
    }
    const state::AccountState account = state::account_snapshot();
    const std::array<std::uint32_t, 5>* armour = nullptr;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (!account.characters[index].selected) {
            continue;
        }
        switch (account.characters[index].characterClass) {
        case state::CharacterClass::hunter:
            armour = &kHunterArmour;
            break;
        case state::CharacterClass::warlock:
            armour = &kWarlockArmour;
            break;
        case state::CharacterClass::titan:
        default:
            armour = &kTitanArmour;
            break;
        }
        break;
    }
    if (armour != nullptr) {
        for (const std::uint32_t hash : *armour) {
            hashes[hashCount++] = hash;
        }
    }

    std::array<std::byte, sizeof(std::uint32_t)> randomBytes{};
    if (!middleware::crypto::random::fill(randomBytes)) {
        return {};
    }
    std::uint32_t randomValue = 0;
    for (std::size_t index = 0; index < randomBytes.size(); ++index) {
        randomValue |= std::to_integer<std::uint32_t>(randomBytes[index]) << (index * 8U);
    }
    const std::size_t first = randomValue % hashCount;
    for (std::size_t offset = 0; offset < hashCount; ++offset) {
        const std::uint32_t hash = hashes[(first + offset) % hashCount];
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(hash, definition)) {
            continue;
        }
        state::PendingItemAcquisition acquisition{};
        if (!state::prepare_item_acquisition_for_item(definition.definitionIndex, acquisition)) {
            continue;
        }
        if (!bap::arm_world_item_acquisition(acquisition)) {
            return {hash, definition.definitionIndex, false};
        }
        return {hash, definition.definitionIndex, true};
    }
    return {};
}

/** Reports and resolves the live world context for an incident whose packet has no egg id. */
[[nodiscard]] EggResolution resolve_egg_context() noexcept {
    namespace activity = state::activity;
    const client::player::position::Snapshot player = client::player::position::snapshot();
    const std::uint64_t sessionId =
        activity::membership::live_region_session(activity::kAbsentSessionId);
    activity::destination::DestinationSelection selection{};
    state::build_data::scenarios::Definition layout{};
    const bool hasDestination = sessionId != activity::kAbsentSessionId
                                && activity::destination::snapshot(sessionId, selection);
    const std::string_view packageName{
        reinterpret_cast<const char*>(selection.packageName.data()), selection.packageNameLength};
    const bool hasLayout = hasDestination
                           && state::build_data::find_scenario_layout(packageName, layout);
    const std::string_view stem{layout.spawnStem.data(), layout.spawnStemLength};
    state::build_data::spawn_sets::Point point{};
    float distance = 0.0F;
    const bool hasSpawn = player.present && hasLayout
                          && state::build_data::find_nearest_spawn_point(
                              stem, player.position, point, distance);
    state::build_data::hash_names::Name name{};
    const bool hasName = hasSpawn && state::build_data::find_hash_name(point.nameHash, name);
    const std::string_view spawnName{name.name.data(), name.nameLength};
    const std::int32_t region = sessionId == activity::kAbsentSessionId
                                    ? -1
                                    : activity::membership::reported_region(sessionId);
    report(core::log::Level::info,
           "ev=activity stage=lore_egg_context position=%s x=%.3f y=%.3f z=%.3f "
           "session=0x%016llX region=%d package=%.*s stem=%.*s spawn=%s hash=0x%08X "
           "name=%.*s distance=%.3f",
           player.present ? "present" : "absent",
           static_cast<double>(player.position[0]),
           static_cast<double>(player.position[1]),
           static_cast<double>(player.position[2]),
           static_cast<unsigned long long>(sessionId),
           region,
           static_cast<int>(packageName.size()),
           packageName.data(),
           static_cast<int>(stem.size()),
           stem.data(),
           hasSpawn ? "found" : "absent",
           hasSpawn ? point.nameHash : 0U,
           static_cast<int>(hasName ? spawnName.size() : 0U),
           hasName ? spawnName.data() : "",
           static_cast<double>(hasSpawn ? distance : 0.0F));

    // The community checklist groups its Egg #1 under Gardens of Esila / Imponent II while naming
    // the physical location "Divalian Mists - Next to spawn". The reported region changes across
    // loads at the same coordinates, so the stable nearest-spawn identity resolves that egg.
    constexpr std::string_view kDreamingCityFreeroam = "dreaming_city_freeroam";
    constexpr std::uint32_t kDivalianSpawnHash = 0xE3D5F2D5U;
    constexpr float kDivalianSpawnRadius = 16.0F;
    constexpr std::uint16_t kImponentTwoRecord = 40;
    if (player.present && packageName == kDreamingCityFreeroam
        && hasSpawn && point.nameHash == kDivalianSpawnHash
        && distance <= kDivalianSpawnRadius) {
        return {state::lore::advance_record(kImponentTwoRecord), kImponentTwoRecord, true};
    }
    return {};
}

} // namespace

/** Frames a sensor sense update and reports its epoch. */
Framed frame_sense_update(const message::Request& request) noexcept {
    namespace sense = message::sense_update;
    sense::SenseUpdate update{};
    std::size_t consumed = 0;
    if (!sense::parse_sense_update(request.payload, update, consumed)) {
        return {report_malformed("sense", request), consumed};
    }
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
           "optional=%u payload=%u hdrbits=%u",
           incident::verdict_name(verdict),
           parsed.primaryTarget,
           parsed.extraTargetCount,
           parsed.selectorLength,
           static_cast<unsigned>(parsed.hasOptionalBlock),
           parsed.payloadLength,
           parsed.headerBits);
    // Resolve only the authored identity the incident names. A bubble cannot distinguish the
    // collectible families that coexist in the Dreaming City, so unknown identities grant nothing.
    if (accepted) {
        // Preserve the common-header size gate before acting on the incident.
        const std::span<const std::byte> body{parsed.payload.data(), parsed.payloadLength};
        if (body.size() >= 13) {
            struct Resolution {
                state::lore::GrantOutcome outcome{state::lore::GrantOutcome::recordNotFound};
                bool resolved{};
            };
            constexpr std::uint32_t kCorruptedEggTarget = 693U;
            constexpr std::uint32_t kCorruptedEggNameHash = 0x179A5E15U;
            constexpr std::uint32_t kCorruptedEggLane4 = 0x0A06FFFFU;
            const auto resolve = [](std::uint32_t target) noexcept -> Resolution {
                state::build_data::sobjects::Definition definition{};
                if (!state::build_data::sobjects::find(static_cast<std::uint16_t>(target), definition)) {
                    return {};
                }
                if (definition.typeCode == 10) {
                    const auto outcome = state::lore::grant_record(definition.recordRow());
                    return {outcome, outcome != state::lore::GrantOutcome::recordNotFound
                                          && outcome != state::lore::GrantOutcome::notAChapter};
                }
                if (definition.typeCode == 2) {
                    constexpr std::uint16_t kDroneFirstOrdinal = 2455U;
                    constexpr std::uint16_t kDroneLastOrdinal = 2470U;
                    // Four Forsaken Prince chapters are campaign rewards rather than Fallen
                    // device collectibles, so the collectible records are not contiguous.
                    constexpr std::array<std::uint16_t, 16> kDroneRecords{
                        740U, 741U, 742U, 744U, 746U, 747U, 748U, 749U,
                        750U, 751U, 752U, 753U, 754U, 755U, 756U, 757U,
                    };
                    constexpr std::uint16_t kGhostFirstOrdinal = 2471U;
                    constexpr std::uint16_t kGhostLastOrdinal = 2493U;
                    constexpr std::uint16_t kGhostFirstRecord = 802U;
                    constexpr std::uint16_t kCrystalFirstOrdinal = 2494U;
                    constexpr std::uint16_t kCrystalLastOrdinal = 2516U;
                    constexpr std::uint16_t kCrystalFirstRecord = 778U;
                    constexpr std::uint16_t kBoneFirstOrdinal = 2517U;
                    constexpr std::uint16_t kBoneLastOrdinal = 2532U;
                    constexpr std::uint16_t kBoneFirstRecord = 759U;
                    const std::uint16_t ordinal = definition.loreObjectOrdinal();
                    std::uint16_t record = 0;
                    if (ordinal >= kDroneFirstOrdinal && ordinal <= kDroneLastOrdinal) {
                        record = kDroneRecords[ordinal - kDroneFirstOrdinal];
                    } else if (ordinal >= kGhostFirstOrdinal && ordinal <= kGhostLastOrdinal) {
                        record = static_cast<std::uint16_t>(
                            kGhostFirstRecord + ordinal - kGhostFirstOrdinal);
                    } else if (ordinal >= kCrystalFirstOrdinal && ordinal <= kCrystalLastOrdinal) {
                        record = static_cast<std::uint16_t>(
                            kCrystalFirstRecord + ordinal - kCrystalFirstOrdinal);
                    } else if (ordinal >= kBoneFirstOrdinal && ordinal <= kBoneLastOrdinal) {
                        record = static_cast<std::uint16_t>(
                            kBoneFirstRecord + ordinal - kBoneFirstOrdinal);
                    } else {
                        return {};
                    }
                    const auto outcome = state::lore::grant_record(record);
                    return {outcome, true};
                }
                return {};
            };

            Resolution resolution = resolve(parsed.primaryTarget);
            std::uint32_t resolvedTarget = parsed.primaryTarget;
            if (!resolution.resolved) {
                for (std::uint32_t index = 0; index < parsed.extraTargetCount; ++index) {
                    resolution = resolve(parsed.extraTargets[index]);
                    if (resolution.resolved) {
                        resolvedTarget = parsed.extraTargets[index];
                        break;
                    }
                }
            }

            state::build_data::sobjects::Definition primary{};
            const bool primaryFound = state::build_data::sobjects::find(
                static_cast<std::uint16_t>(parsed.primaryTarget), primary);
            const bool isCorruptedEgg = parsed.primaryTarget == kCorruptedEggTarget && primaryFound
                                        && primary.typeCode == 3
                                        && primary.nameHash == kCorruptedEggNameHash
                                        && primary.lane4 == kCorruptedEggLane4;
            if (resolution.outcome == state::lore::GrantOutcome::granted
                || resolution.outcome == state::lore::GrantOutcome::progressed) {
                bap::arm_account_resync_everywhere();
            }
            if (resolution.resolved) {
                state::build_data::sobjects::Definition exact{};
                const bool exactFound = state::build_data::sobjects::find(
                    static_cast<std::uint16_t>(resolvedTarget), exact);
                report(resolution.outcome == state::lore::GrantOutcome::granted
                           ? core::log::Level::info
                           : core::log::Level::debug,
                       "ev=activity stage=lore path=exact target=%u result=%s record=%u "
                       "type=%d hash=0x%08X lanes=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X",
                       resolvedTarget, state::lore::grant_outcome_name(resolution.outcome),
                       static_cast<unsigned>(
                           resolution.outcome == state::lore::GrantOutcome::granted
                                   || resolution.outcome == state::lore::GrantOutcome::progressed
                               ? state::lore::last_granted_record()
                               : 0),
                       exactFound ? exact.typeCode : 0,
                       exactFound ? exact.nameHash : 0U,
                       exactFound ? exact.lanes[0] : 0U,
                       exactFound ? exact.lanes[1] : 0U,
                       exactFound ? exact.lanes[2] : 0U,
                       exactFound ? exact.lanes[3] : 0U,
                       exactFound ? exact.lanes[4] : 0U,
                       exactFound ? exact.lanes[5] : 0U,
                       exactFound ? exact.lanes[6] : 0U,
                       exactFound ? exact.lanes[7] : 0U);
            } else if (isCorruptedEgg) {
                const EggResolution egg = resolve_egg_context();
                const EggLootResolution loot = grant_random_egg_loot();
                if (egg.resolved
                    && (egg.outcome == state::lore::GrantOutcome::granted
                        || egg.outcome == state::lore::GrantOutcome::progressed)) {
                    bap::arm_account_resync_everywhere();
                }
                report(core::log::Level::info,
                       "ev=activity stage=lore path=egg result=%s target=%u type=%d "
                       "lane4=0x%08X record=%u loot=%s item_hash=0x%08X item_index=%u",
                       egg.resolved ? state::lore::grant_outcome_name(egg.outcome) : "unresolved",
                       parsed.primaryTarget,
                       primary.typeCode,
                       primary.lane4,
                       static_cast<unsigned>(egg.resolved ? egg.record : 0),
                       loot.granted ? "queued" : "failed",
                       loot.definitionHash,
                       loot.definitionIndex);
                if (core::log::accepts(core::log::Channel::server, core::log::Level::info)) {
                    std::array<char, core::log::kLineCapacity> line{};
                    const int prefix = std::snprintf(line.data(), line.size(),
                                                     "ev=activity stage=lore_egg payload_bytes=%u "
                                                     "payload_hex=",
                                                     parsed.payloadLength);
                    if (prefix > 0 && static_cast<std::size_t>(prefix) < line.size()) {
                        std::size_t length = static_cast<std::size_t>(prefix);
                        (void)core::log::append_hex(line, length, body);
                        const int requestPrefix = std::snprintf(
                            line.data() + length, line.size() - length,
                            " request_bytes=%zu request_hex=", request.payload.size());
                        if (requestPrefix > 0
                            && static_cast<std::size_t>(requestPrefix) < line.size() - length) {
                            length += static_cast<std::size_t>(requestPrefix);
                            (void)core::log::append_hex(line, length, request.payload);
                        }
                        core::log::write(core::log::Channel::server, core::log::Level::info,
                                         {line.data(), length});
                    }
                }
            } else {
                bool contextResolved = false;
                if (parsed.primaryTarget == 3539U) {
                    namespace activity = state::activity;
                    const auto player = client::player::position::snapshot();
                    const std::uint64_t sessionId = activity::membership::live_region_session(
                        activity::kAbsentSessionId);
                    activity::destination::DestinationSelection selection{};
                    (void)activity::destination::snapshot(sessionId, selection);
                    const std::string_view packageName{
                        reinterpret_cast<const char*>(selection.packageName.data()),
                        selection.packageNameLength};
                    report(core::log::Level::info,
                           "ev=activity stage=lore_generic_context target=%u type=%d "
                           "hash=0x%08X lane4=0x%08X position=%s "
                           "x=%.3f y=%.3f z=%.3f region=%d package=%.*s extras=%u",
                           parsed.primaryTarget,
                           primaryFound ? primary.typeCode : 0,
                           primaryFound ? primary.nameHash : 0U,
                           primaryFound ? primary.lane4 : 0U,
                           player.present ? "present" : "absent",
                           static_cast<double>(player.position[0]),
                           static_cast<double>(player.position[1]),
                           static_cast<double>(player.position[2]),
                           sessionId == activity::kAbsentSessionId
                               ? -1
                               : activity::membership::reported_region(sessionId),
                           static_cast<int>(packageName.size()),
                           packageName.data(),
                           parsed.extraTargetCount);
                    for (std::uint32_t index = 0; index < parsed.extraTargetCount; ++index) {
                        state::build_data::sobjects::Definition extra{};
                        const bool found = state::build_data::sobjects::find(
                            static_cast<std::uint16_t>(parsed.extraTargets[index]), extra);
                        report(core::log::Level::info,
                               "ev=activity stage=lore_generic_extra slot=%u target=%u found=%u "
                               "type=%d hash=0x%08X lanes=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X",
                               index,
                               parsed.extraTargets[index],
                               found ? 1U : 0U,
                               found ? extra.typeCode : 0,
                               found ? extra.nameHash : 0U,
                               found ? extra.lanes[0] : 0U,
                               found ? extra.lanes[1] : 0U,
                               found ? extra.lanes[2] : 0U,
                               found ? extra.lanes[3] : 0U,
                               found ? extra.lanes[4] : 0U,
                               found ? extra.lanes[5] : 0U,
                               found ? extra.lanes[6] : 0U,
                               found ? extra.lanes[7] : 0U);
                    }

                    // Dust scans share one target identity. The physical scan position selects
                    // the exact lore entry across the Derelict and Reckoning spaces.
                    struct DustScan {
                        std::array<float, 3> position;
                        std::uint16_t record;
                    };
                    constexpr std::string_view kDerelictPackage = "pandora_freeroam";
                    constexpr std::array<DustScan, 9> kDustScans{{
                        {{{-40.861F, 147.410F, -2312.313F}}, 1571U}, // The Bone, Derelict
                        {{{-681.393F, -859.831F, -8.590F}}, 1575U}, // The Declaration, first arena
                        {{{2.871F, 236.277F, -2306.442F}}, 1569U},  // The Red Box, Derelict
                        {{{7.362F, 237.020F, -2306.704F}}, 1572U},   // The Kell, Derelict
                        {{{9.792F, 149.124F, -2319.657F}}, 1570U},   // The Stacks, Reckoning
                        {{{-205.802F, -80.132F, -11.900F}}, 1574U}, // The Gate, Reckoning
                        {{{-249.759F, 6.664F, -17.853F}}, 1573U},   // The Leviathan, Reckoning
                        {{{-876.144F, -874.570F, 11.781F}}, 1576U}, // The Nine, Reckoning
                        {{{-1323.416F, -534.063F, -298.928F}}, 1577U}, // The Witch, Reckoning
                    }};
                    constexpr float kDustScanRadiusSquared = 36.0F;
                    if (player.present && packageName == kDerelictPackage) {
                        for (const DustScan& scan : kDustScans) {
                            const float dx = player.position[0] - scan.position[0];
                            const float dy = player.position[1] - scan.position[1];
                            const float dz = player.position[2] - scan.position[2];
                            if (dx * dx + dy * dy + dz * dz > kDustScanRadiusSquared) {
                                continue;
                            }
                            const auto outcome = state::lore::grant_record(scan.record);
                            contextResolved = outcome != state::lore::GrantOutcome::recordNotFound
                                              && outcome != state::lore::GrantOutcome::notAChapter;
                            if (outcome == state::lore::GrantOutcome::granted) {
                                bap::arm_account_resync_everywhere();
                            }
                            report(core::log::Level::info,
                                   "ev=activity stage=lore path=position target=%u package=%.*s "
                                   "record=%u result=%s",
                                   parsed.primaryTarget,
                                   static_cast<int>(packageName.size()), packageName.data(),
                                   static_cast<unsigned>(scan.record),
                                   state::lore::grant_outcome_name(outcome));
                            break;
                        }
                    }

                    // Confessions vases have no per-object target. Their interaction positions are
                    // stable across captures, so each measured centre resolves its authored entry.
                    struct ConfessionsVase {
                        std::array<float, 3> position;
                        std::uint16_t record;
                    };
                    constexpr std::string_view kMenageriePackage = "caluseum_experience";
                    constexpr std::array<ConfessionsVase, 8> kConfessionsVases{{
                        {{30.559F, 31.233F, -2.185F}, 1708U},   // Entry I, Lamplighting
                        {{57.683F, 8.844F, -43.121F}, 1709U},  // Entry II, The Hunted
                        {{61.939F, 220.947F, 2.439F}, 1710U},  // Entry III, Royal Theatre
                        {{143.082F, -23.093F, 11.629F}, 1711U}, // Entry IV, War Beast statue
                        {{109.207F, 211.327F, -144.309F}, 1712U}, // Entry V, Gauntlet
                        {{403.643F, -5.111F, 6.669F}, 1713U},  // Entry VI, Crown of Sorrow
                        {{947.203F, 2.456F, 131.591F}, 1714U},  // Entry VII, Crown of Sorrow
                        {{1138.881F, 89.454F, 94.136F}, 1715U}, // Entry VIII, Crown of Sorrow
                    }};
                    constexpr float kConfessionsVaseRadiusSquared = 36.0F;
                    constexpr std::string_view kTributeHallPackage = "trophy_hall_freeroam";
                    constexpr std::array<float, 3> kConfessionsEntryNinePosition{
                        25.642F, 0.012F, 5.922F};
                    if (player.present && packageName == kTributeHallPackage) {
                        const float dx = player.position[0] - kConfessionsEntryNinePosition[0];
                        const float dy = player.position[1] - kConfessionsEntryNinePosition[1];
                        const float dz = player.position[2] - kConfessionsEntryNinePosition[2];
                        if (dx * dx + dy * dy + dz * dz <= kConfessionsVaseRadiusSquared) {
                            constexpr std::uint16_t kConfessionsEntryNineRecord = 1716U;
                            const auto outcome =
                                state::lore::grant_record(kConfessionsEntryNineRecord);
                            contextResolved = outcome != state::lore::GrantOutcome::recordNotFound
                                              && outcome != state::lore::GrantOutcome::notAChapter;
                            if (outcome == state::lore::GrantOutcome::granted) {
                                bap::arm_account_resync_everywhere();
                            }
                            report(core::log::Level::info,
                                   "ev=activity stage=lore path=position target=%u package=%.*s "
                                   "record=%u result=%s",
                                   parsed.primaryTarget,
                                   static_cast<int>(packageName.size()), packageName.data(),
                                   static_cast<unsigned>(kConfessionsEntryNineRecord),
                                   state::lore::grant_outcome_name(outcome));
                        }
                    } else if (player.present && packageName == kMenageriePackage) {
                        for (const ConfessionsVase& vase : kConfessionsVases) {
                            const float dx = player.position[0] - vase.position[0];
                            const float dy = player.position[1] - vase.position[1];
                            const float dz = player.position[2] - vase.position[2];
                            if (dx * dx + dy * dy + dz * dz > kConfessionsVaseRadiusSquared) {
                                continue;
                            }
                            const auto outcome = state::lore::grant_record(vase.record);
                            contextResolved = outcome != state::lore::GrantOutcome::recordNotFound
                                              && outcome != state::lore::GrantOutcome::notAChapter;
                            if (outcome == state::lore::GrantOutcome::granted) {
                                bap::arm_account_resync_everywhere();
                            }
                            report(core::log::Level::info,
                                   "ev=activity stage=lore path=position target=%u package=%.*s "
                                   "record=%u result=%s",
                                   parsed.primaryTarget,
                                   static_cast<int>(packageName.size()),
                                   packageName.data(),
                                   static_cast<unsigned>(vase.record),
                                   state::lore::grant_outcome_name(outcome));
                            break;
                        }
                    }
                }
                if (!contextResolved) {
                    report(core::log::Level::debug,
                           "ev=activity stage=lore path=unresolved target=%u extra=%u",
                           parsed.primaryTarget, parsed.extraTargetCount);
                }
            }
        }
    }

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
