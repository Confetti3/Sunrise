#include "physics_session.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <new>
#include <string>

#include "../../../../core/settings/settings.h"
#include "../../../../middleware/content/packages/tables/region_reader.h"
#include "../../../../middleware/bap/activity_message/glimmer_extraction_contract.h"
#include "../../../../state/activity/entity_slots/runtime.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/gameplay/physics/runtime.h"
#include "../../../bap/runtime.h"
#include "../../gameplay_log.h"
#include "../../group/group_host.h"
#include "../../group/group_host_sessions.h"
#include "../../mission/compiled_mission_policy.h"
#include "../../mission/content_step_queue.h"
#include "../../mission/enemy_wave_queue.h"
#include "../../mission/mission_compiler.h"
#include "../../peer/peer_transport.h"
#include "../replication/world_coordinator.h"
#include "bubble_host.h"
#include "mission_signal_queue.h"
#include "runtime.h"

namespace sunrise::server::gameplay::physics::host::session {
namespace {

namespace replica = state::gameplay::physics;
namespace signal_queue = mission_signal_queue;

/** Worlds this bridge holds open. The host bounds its own table at the same number. */
constexpr std::size_t kSessionCapacity = kWorldCapacity;
/** Admitted rows one snapshot reads. The group table holds no more than this. */
constexpr std::size_t kAdmittedCapacity = 8;
/** Host-session rows one snapshot reads. */
constexpr std::size_t kHostRowCapacity = 8;
/** The world runs at 30 Hz, so one tick is due every 33 ms. */
constexpr std::uint64_t kTickIntervalMs = 33;
/** Empty scene scale. Nothing reads it until an actor carries a transform. */
constexpr float kMillimetersPerUnit = 1000.0F;
/**
 * Content build stamped on the scene, its manifest and its navigation scene.
 * The backend refuses a zero one. This scene loads no content, so the number's only job is to make
 * those three agree; it becomes a real build id once a scene carries geometry.
 * TODO: no reader outside that agreement check. Take it from `state::build_data::BuildIdentity`
 * when a scene loads content.
 */
constexpr std::uint64_t kSceneContentBuild =
    middleware::bap::activity_message::glimmer_extraction::kBuildId;
/**
 * Worker wake interval. `tick_bound` paces the 30 Hz step itself, so this only bounds the jitter.
 * A world tick is never due more than this long after its deadline.
 */
constexpr std::uint64_t kWorkerSliceMs = 8;
/**
 * How often the worker rebuilds the bound set from the admitted peers.
 * Every wake would take three shared locks a hundred times a second to notice a join that takes
 * seconds to arrive.
 */
constexpr std::uint64_t kReconcileIntervalMs = 250;
/** A refused open waits this long before it is tried again. */
constexpr std::uint64_t kOpenRetryMs = 5'000;
/** Attempts one host generation gets before the bridge stops trying it. */
constexpr std::uint32_t kOpenAttemptLimit = 3;

/** One admitted public group or retained private ActivityClient bound to one open world. */
struct Bound final {
    WorldHandle world{};
    HostPeerHandle peer{};
    replica::ContextHandle context{};
    replica::PeerReplicaHandle replicaHandle{};
    state::activity::SessionBinding retainedSource{};
    std::uint64_t groupSessionId{};
    std::uint64_t activitySessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t waveDeliverySessionId{};
    std::uint64_t waveDeliveryGeneration{};
    std::uint64_t waveDeliveryBindingGeneration{};
    std::uint64_t nextTick{};
    std::uint64_t ticks{};
    std::unique_ptr<mission::CompiledMissionPolicy> missionPolicy{};
    bool privateSource{};
    bool occupied{};
};

/**
 * One host generation this bridge failed to open a world for.
 * Without this the retry runs every service slice. That is what it did: 12,156 refused opens in
 * one run, each one initializing and clearing the whole physics backend, until the game's main
 * loop stalled.
 */
struct Attempt final {
    std::uint64_t groupSessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t nextAttempt{};
    std::uint32_t failures{};
    bool occupied{};
};

/** The coordinators are large, so the whole table is one heap allocation made on first use. */
struct Storage final {
    std::array<Bound, kSessionCapacity> bound{};
    std::array<Attempt, kAdmittedCapacity> attempts{};
    Attempt privateAttempt{};
    server::bap::ActivitySnapshot authorizedPrivateSource{};
    std::array<replication::WorldCoordinator, kSessionCapacity> coordinators{};
};

std::unique_ptr<Storage> g_storage{};
/** The worker, or null while the bridge is off. Only the pump thread writes these two. */
HANDLE g_thread{};
/** Manual-reset stop signal. It doubles as the worker's sleep, so a stop is never waited out. */
HANDLE g_stop{};
/**
 * Separates one bind of a group session from the next.
 * State refuses a zero view epoch and uses these fields only to fail a stale handle closed, so a
 * process-local counter is the whole requirement. It is not a wire value.
 */
std::uint64_t g_bindEpoch = 0;
std::atomic_uint64_t g_reloadRequested{};
std::atomic_uint64_t g_reloadCompleted{};
std::atomic_uint64_t g_triggerId{};
std::atomic_uint64_t g_triggerRequested{};
std::atomic_uint64_t g_triggerCompleted{};
std::atomic_uint64_t g_programHash{};
std::atomic_uint32_t g_missionState{};
std::atomic_uint32_t g_objective0{};
std::atomic_uint32_t g_activatedWaveCount{};
std::atomic_uint32_t g_activeWorlds{};
std::uint64_t g_reloadApplied{};

/** @return The table, allocated on first use, or null when it cannot be allocated. */
[[nodiscard]] Storage* storage() noexcept {
    if (g_storage == nullptr) {
        g_storage.reset(new (std::nothrow) Storage{});
    }
    return g_storage.get();
}

[[nodiscard]] std::filesystem::path mission_directory() noexcept {
    std::array<wchar_t, 32'768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) {
        return {};
    }
    return std::filesystem::path(path.data()).parent_path() / L"Sunrise" / L"missions";
}

[[nodiscard]] bool destination_for(
    const state::activity::destination::DestinationSelection& selected,
    std::string& destination) noexcept {
    if (selected.packageNameLength == 0
        || selected.packageNameLength > selected.packageName.size()) {
        return false;
    }
    destination.clear();
    destination.reserve(selected.packageNameLength);
    for (std::size_t index = 0; index < selected.packageNameLength; ++index) {
        destination.push_back(static_cast<char>(selected.packageName[index]));
    }
    return true;
}

/** Accepts only the exact forced build-86657 Trostland destination source. */
[[nodiscard]] bool private_mission_source(const server::bap::ActivitySnapshot& snapshot,
                                          std::string& destination) noexcept {
    namespace glimmer = middleware::bap::activity_message::glimmer_extraction;
    const auto& binding = snapshot.binding;
    const auto& selected = binding.destination;
    return binding.sessionId != state::activity::kAbsentSessionId
           && binding.createdRevision != state::activity::kInvalidRevision
           && snapshot.bindingGeneration != 0
           && destination_for(selected, destination)
           && destination == "edz_freeroam"
           && selected.hasArrivalBubbleOverride
           && selected.arrivalBubbleOverride == glimmer::kBubble
           && selected.hasSliceSetOverride
           && selected.sliceSetOverride == glimmer::kSlice
           && state::activity::binding_matches(binding);
}

/** @return True when two snapshots name the same private BAP and State lifetime. */
[[nodiscard]] bool same_private_lifetime(const server::bap::ActivitySnapshot& left,
                                         const server::bap::ActivitySnapshot& right) noexcept {
    return left.bindingGeneration == right.bindingGeneration
           && left.binding.sessionId == right.binding.sessionId
           && left.binding.createdRevision == right.binding.createdRevision;
}

[[nodiscard]] bool configure_mission_scene(const mission::MissionProgram& program,
                                           std::uint32_t bubbleIndex,
                                           backend::SceneSpec& scene) noexcept {
    for (std::size_t index = 0; index < program.interactionCount; ++index) {
        const auto& interaction = program.interactions[index];
        if (interaction.bubble != bubbleIndex) return false;
    }
    if (program.interactionCount != 0) {
        const auto& interaction = program.interactions[0];
        scene.bounds.minimum.x = interaction.transform.position.x - interaction.extents.x;
        scene.bounds.minimum.y = interaction.transform.position.y - interaction.extents.y;
        scene.bounds.minimum.z = interaction.transform.position.z - interaction.extents.z;
        scene.bounds.maximum.x = interaction.transform.position.x + interaction.extents.x;
        scene.bounds.maximum.y = interaction.transform.position.y + interaction.extents.y;
        scene.bounds.maximum.z = interaction.transform.position.z + interaction.extents.z;
    }
    return true;
}

[[nodiscard]] bool bubble_for_region(std::int32_t regionIndex,
                                     std::uint32_t& bubbleIndex) noexcept {
    namespace tables = middleware::content::packages::tables;
    if (regionIndex < 0) return false;
    const auto value = static_cast<std::uint32_t>(regionIndex);
    if (value >= tables::kRegionIndexBound || value % tables::kSliceSetIndexFactor != 0) {
        return false;
    }
    bubbleIndex = value / tables::kSliceSetIndexFactor;
    return true;
}

/**
 * Finds the host row that published one admitted group session.
 * @param rows Copied host-session rows.
 * @param count Rows copied.
 * @param groupSessionId Group session the peer joined.
 * @return The row, or null when no host row published it.
 */
[[nodiscard]] const group::HostSessionRow*
find_host_row(const std::array<group::HostSessionRow, kHostRowCapacity>& rows,
              std::size_t count,
              std::uint64_t groupSessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (rows[index].groupSessionId == groupSessionId) {
            return &rows[index];
        }
    }
    return nullptr;
}

/** @return True when one group session is still admitted and its join has finished. */
[[nodiscard]] bool still_joined(const std::array<group::AdmittedRow, kAdmittedCapacity>& rows,
                                std::size_t count,
                                std::uint64_t groupSessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (rows[index].sessionId == groupSessionId && rows[index].joinComplete) {
            return true;
        }
    }
    return false;
}

/**
 * Finds or claims the attempt record for one host generation.
 * A different generation is a different activity, so its record starts clean.
 * @param table Bridge table.
 * @param groupSessionId Group session being opened.
 * @param hostGeneration Host-row generation being opened.
 * @return The record, or null when the table is full.
 */
[[nodiscard]] Attempt*
attempt_for(Storage& table, std::uint64_t groupSessionId, std::uint64_t hostGeneration) noexcept {
    Attempt* free = nullptr;
    for (Attempt& record : table.attempts) {
        if (record.occupied && record.groupSessionId == groupSessionId) {
            if (record.hostGeneration != hostGeneration) {
                record = {groupSessionId, hostGeneration, 0, 0, true};
            }
            return &record;
        }
        if (free == nullptr && !record.occupied) {
            free = &record;
        }
    }
    if (free != nullptr) {
        *free = {groupSessionId, hostGeneration, 0, 0, true};
    }
    return free;
}

/** Forgets the attempt record for one group session, so a later bind starts clean. */
void clear_attempt(Storage& table, std::uint64_t groupSessionId) noexcept {
    for (Attempt& record : table.attempts) {
        if (record.occupied && record.groupSessionId == groupSessionId) {
            record = {};
        }
    }
}

/** @return The slot already bound to one group session, or the capacity when none is. */
[[nodiscard]] std::size_t find_bound(const Storage& table, std::uint64_t groupSessionId) noexcept {
    for (std::size_t index = 0; index < table.bound.size(); ++index) {
        if (table.bound[index].occupied && !table.bound[index].privateSource
            && table.bound[index].groupSessionId == groupSessionId) {
            return index;
        }
    }
    return table.bound.size();
}

/** @return The retained private policy slot, or the capacity when none is open. */
[[nodiscard]] std::size_t find_private_bound(const Storage& table) noexcept {
    for (std::size_t index = 0; index < table.bound.size(); ++index) {
        if (table.bound[index].occupied && table.bound[index].privateSource) {
            return index;
        }
    }
    return table.bound.size();
}

/** Releases every owner one bound slot holds, in reverse order, then clears it. */
void close_bound(Storage& table, std::size_t slot) noexcept {
    Bound& entry = table.bound[slot];
    const bool hadMissionPolicy = entry.missionPolicy != nullptr;
    mission::cancel_enemy_waves(entry.waveDeliverySessionId,
                                entry.waveDeliveryGeneration,
                                entry.waveDeliveryBindingGeneration);
    mission::cancel_content_steps(entry.waveDeliverySessionId,
                                  entry.waveDeliveryGeneration,
                                  entry.waveDeliveryBindingGeneration);
    signal_queue::unregister_scope(entry.waveDeliverySessionId,
                            entry.waveDeliveryGeneration,
                            entry.waveDeliveryBindingGeneration);
    BubbleHost* host = runtime::instance();
    if (!entry.privateSource) {
        // The planner belongs to the host peer, so the coordinator has to let it go before the
        // peer does. Borrowing it after unbind_peer would name a retired slot.
        PeerServiceAccess services{};
        if (host != nullptr
            && host->peer_services(entry.world, entry.peer, services) == HostStatus::success
            && services.replication != nullptr) {
            static_cast<void>(table.coordinators[slot].unregister_peer(*services.replication));
        }
        if (!table.coordinators[slot].unbind()) {
            report(core::log::Level::warn,
                   "ev=physics stage=world result=fail session=0x%016llX "
                   "reason=coordinator_busy",
                   static_cast<unsigned long long>(entry.groupSessionId));
        }
        if (host != nullptr) {
            static_cast<void>(host->unbind_peer(entry.world, entry.peer));
        }
        static_cast<void>(replica::retire_peer_replica(entry.replicaHandle));
        static_cast<void>(replica::reset_context(entry.context));
    }
    if (host != nullptr) {
        static_cast<void>(host->close_world(entry.world));
    }
    if (entry.privateSource) {
        state::activity::release_binding(entry.retainedSource);
    }
    report(core::log::Level::info,
           "ev=physics stage=world result=closed session=0x%016llX activity=0x%016llX "
           "ticks=%llu mode=%s",
           static_cast<unsigned long long>(entry.groupSessionId),
           static_cast<unsigned long long>(entry.activitySessionId),
           static_cast<unsigned long long>(entry.ticks),
           entry.privateSource ? "private_source" : "public_peer");
    entry = {};
    if (hadMissionPolicy
        && g_activeWorlds.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_triggerCompleted.store(g_triggerRequested.load(std::memory_order_acquire),
                                 std::memory_order_release);
        g_programHash.store(0, std::memory_order_release);
        g_missionState.store(0, std::memory_order_release);
        g_objective0.store(0, std::memory_order_release);
        g_activatedWaveCount.store(0, std::memory_order_release);
        signal_queue::reset();
    }
}

/**
 * Publishes one State replica context for an activity that already committed its lease.
 * @param row Host row naming the activity session.
 * @param handle Receives the context handle on success.
 * @return True when both lease masks were read and the context published.
 */
[[nodiscard]] bool publish_context(const group::HostSessionRow& row,
                                   replica::ContextHandle& handle) noexcept {
    replica::ActivityReplicaContext context{};
    if (!state::activity::entity_slots::lease_masks(
            row.hostSessionId, context.clientLeaseMask, context.serverReserveMask)) {
        return false;
    }
    context.activitySessionId = row.hostSessionId;
    context.currentBubble = static_cast<std::uint32_t>(row.regionIndex);
    // Every session reaching this bridge joined a published public host row.
    context.role = replica::ReplicaRole::publicTarget;
    // The patch epoch stays zero here and in the common binding below, so the two agree and State
    // accepts them. Only frame encoding compares it against the client's own root, and nothing
    // encodes a frame yet.
    return replica::publish_context(context, handle);
}

/**
 * Opens one world, binds its peer, and registers both with the coordinator.
 * @param table Bridge table.
 * @param slot Free bridge slot.
 * @param admitted Admitted row for the peer.
 * @param row Host row naming the activity session.
 * @param now Monotonic tick count in milliseconds.
 * @return True when the world opened. False is a refusal the caller must back off from.
 */
[[nodiscard]] bool open_bound(Storage& table,
                              std::size_t slot,
                              const group::AdmittedRow& admitted,
                              const group::HostSessionRow& row,
                              std::uint64_t now) noexcept {
    BubbleHost* host = runtime::instance();
    if (host == nullptr) {
        return false;
    }
    peer::LinkIdentity link{};
    if (admitted.joinId == 0 || row.generation == 0 || row.hostSessionId == 0
        || !peer::link_identity(admitted.sessionId, link)) {
        return false;
    }
    Bound entry{};
    if (!publish_context(row, entry.context)) {
        // The lease commits after the join, so this is ordinary for the first slices.
        return false;
    }
    ++g_bindEpoch;
    replica::ViewKey view{};
    view.peer.authenticatedMemberId = admitted.joinId;
    view.peer.associationEpoch = row.generation;
    view.peer.remoteConnectionSequence = link.remoteConnectionSequence;
    view.peer.localConnectionSequence = link.localConnectionSequence;
    view.peer.channelEpoch = g_bindEpoch;
    view.groupSessionId = admitted.sessionId;
    view.viewEpoch = g_bindEpoch;
    if (!replica::publish_peer_replica(entry.context, view, entry.replicaHandle)) {
        static_cast<void>(replica::reset_context(entry.context));
        report(core::log::Level::warn,
               "ev=physics stage=world result=fail session=0x%016llX reason=replica",
               static_cast<unsigned long long>(admitted.sessionId));
        return false;
    }

    std::uint32_t bubbleIndex{};
    if (!bubble_for_region(row.regionIndex, bubbleIndex)) {
        static_cast<void>(replica::retire_peer_replica(entry.replicaHandle));
        static_cast<void>(replica::reset_context(entry.context));
        report(core::log::Level::warn,
               "ev=physics stage=world result=fail session=0x%016llX reason=region region=%d",
               static_cast<unsigned long long>(admitted.sessionId),
               row.regionIndex);
        return false;
    }

    WorldOpenRequest request{};
    request.scene.stableSceneId = row.hostSessionId;
    request.scene.contentBuild = kSceneContentBuild;
    request.scene.bubble = bubbleIndex;
    // The bounds stay the zero box, which the backend accepts. TODO: no body is placed yet, and
    // the first one will be refused for falling outside it. Take the real extents from the
    // destination's scenario layout when an actor carries a transform.
    request.logicalWorldId = row.hostSessionId;
    request.activitySessionId = row.hostSessionId;
    request.ownerEpoch = row.generation;
    request.deterministicSeed = row.hostSessionId;
    request.millimetersPerUnit = kMillimetersPerUnit;
    // Public peer worlds stay scriptless. Authored mission policy is owned only by the exact
    // retained private ActivityClient source below, never by a public target or coordinator view.
    world::IActivityPolicy* policy = nullptr;
    const HostStatus opened = host->open_world(request, policy, entry.world);
    if (opened != HostStatus::success) {
        static_cast<void>(replica::retire_peer_replica(entry.replicaHandle));
        static_cast<void>(replica::reset_context(entry.context));
        report(core::log::Level::warn,
               "ev=physics stage=world result=fail session=0x%016llX status=%u",
               static_cast<unsigned long long>(admitted.sessionId),
               static_cast<unsigned>(opened));
        return false;
    }

    PeerOpenRequest peerRequest{};
    peerRequest.interest.memberId = admitted.joinId;
    peerRequest.interest.viewGeneration = g_bindEpoch;
    peerRequest.interest.activitySessionId = row.hostSessionId;
    peerRequest.common.activitySessionId = row.hostSessionId;
    peerRequest.common.bindingGeneration = row.generation;
    peerRequest.replica = entry.replicaHandle;
    const HostStatus bound = host->bind_peer(entry.world, peerRequest, entry.peer);
    PeerServiceAccess services{};
    const bool registered =
        bound == HostStatus::success
        && host->peer_services(entry.world, entry.peer, services) == HostStatus::success
        && services.replication != nullptr
        && table.coordinators[slot].bind(
            {row.hostSessionId, entry.world.generation, row.generation}, entry.context)
        && table.coordinators[slot].register_peer(*services.replication, peerRequest.interest);
    if (!registered) {
        if (bound == HostStatus::success) {
            static_cast<void>(host->unbind_peer(entry.world, entry.peer));
        }
        static_cast<void>(host->close_world(entry.world));
        static_cast<void>(replica::retire_peer_replica(entry.replicaHandle));
        static_cast<void>(replica::reset_context(entry.context));
        report(core::log::Level::warn,
               "ev=physics stage=world result=fail session=0x%016llX bind=%u reason=peer",
               static_cast<unsigned long long>(admitted.sessionId),
               static_cast<unsigned>(bound));
        return false;
    }

    entry.groupSessionId = admitted.sessionId;
    entry.activitySessionId = row.hostSessionId;
    entry.hostGeneration = row.generation;
    entry.nextTick = now;
    entry.occupied = true;
    table.bound[slot] = std::move(entry);
    report(core::log::Level::info,
           "ev=physics stage=world result=opened session=0x%016llX activity=0x%016llX region=%d "
           "member=0x%016llX worlds=%zu",
           static_cast<unsigned long long>(admitted.sessionId),
           static_cast<unsigned long long>(row.hostSessionId),
           row.regionIndex,
           static_cast<unsigned long long>(admitted.joinId),
           host->world_count());
    return true;
}

/**
 * Opens one policy-only world directly on the retained private ActivityClient source.
 * It deliberately creates no public target, peer, replica, roster tuple, or coordinator view.
 */
[[nodiscard]] bool open_private_bound(Storage& table,
                                      std::size_t slot,
                                      const server::bap::ActivitySnapshot& candidate,
                                      std::uint64_t now) noexcept {
    namespace glimmer = middleware::bap::activity_message::glimmer_extraction;
    BubbleHost* host = runtime::instance();
    std::string destination;
    if (host == nullptr || !core::settings::get().server.activation.missionScriptHost
        || !private_mission_source(candidate, destination)
        || !state::activity::retain_binding(candidate.binding)) {
        return false;
    }

    // Retaining State prevents record eviction. Re-read BAP after the retain so a same-session
    // rebind cannot open a world for the generation that just stopped being private-current.
    server::bap::ActivitySnapshot current{};
    std::string currentDestination;
    const bool stillCurrent = server::bap::snapshot_private_activity(current)
                              && same_private_lifetime(candidate, current)
                              && private_mission_source(current, currentDestination)
                              && currentDestination == destination;
    if (!stillCurrent) {
        state::activity::release_binding(candidate.binding);
        return false;
    }

    mission::MissionCompileResult compiled =
        mission::compile_mission_for_destination(mission_directory(), destination);
    if (compiled.status != mission::MissionCompileStatus::success) {
        state::activity::release_binding(candidate.binding);
        report(core::log::Level::warn,
               "ev=mission stage=compile result=fail destination=%s mode=private_source "
               "reason=%s",
               destination.c_str(),
               compiled.status == mission::MissionCompileStatus::missing
                   ? "missing"
                   : compiled.error.data());
        return false;
    }

    Bound entry{};
    WorldOpenRequest request{};
    request.scene.stableSceneId = candidate.binding.sessionId;
    request.scene.contentBuild = kSceneContentBuild;
    request.scene.bubble = glimmer::kBubble;
    request.logicalWorldId = candidate.binding.sessionId;
    request.activitySessionId = candidate.binding.sessionId;
    request.ownerEpoch = candidate.bindingGeneration;
    request.deterministicSeed = candidate.binding.sessionId;
    request.millimetersPerUnit = kMillimetersPerUnit;
    if (!configure_mission_scene(compiled.program, glimmer::kBubble, request.scene)) {
        state::activity::release_binding(candidate.binding);
        report(core::log::Level::warn,
               "ev=mission stage=select result=fail destination=%s mode=private_source "
               "reason=bubble",
               destination.c_str());
        return false;
    }
    entry.missionPolicy.reset(new (std::nothrow) mission::CompiledMissionPolicy{});
    if (entry.missionPolicy == nullptr
        || !entry.missionPolicy->configure(compiled.program, request.scene.contentBuild)) {
        state::activity::release_binding(candidate.binding);
        report(core::log::Level::warn,
               "ev=mission stage=configure result=fail destination=%s mode=private_source",
               destination.c_str());
        return false;
    }

    server::bap::ActivitySnapshot publishCurrent{};
    std::string publishDestination;
    if (!server::bap::snapshot_private_activity(publishCurrent)
        || !same_private_lifetime(candidate, publishCurrent)
        || !private_mission_source(publishCurrent, publishDestination)
        || publishDestination != destination) {
        state::activity::release_binding(candidate.binding);
        return false;
    }

    const HostStatus opened = host->open_world(request, entry.missionPolicy.get(), entry.world);
    if (opened != HostStatus::success) {
        state::activity::release_binding(candidate.binding);
        report(core::log::Level::warn,
               "ev=physics stage=world result=fail session=0x%016llX status=%u "
               "mode=private_source",
               static_cast<unsigned long long>(candidate.binding.sessionId),
               static_cast<unsigned>(opened));
        return false;
    }

    entry.retainedSource = candidate.binding;
    entry.groupSessionId = candidate.binding.sessionId;
    entry.activitySessionId = candidate.binding.sessionId;
    entry.hostGeneration = candidate.bindingGeneration;
    entry.waveDeliverySessionId = candidate.binding.sessionId;
    entry.waveDeliveryGeneration = candidate.binding.createdRevision;
    entry.waveDeliveryBindingGeneration = candidate.bindingGeneration;
    entry.nextTick = now;
    entry.privateSource = true;
    entry.occupied = true;
    if (!signal_queue::register_scope(entry.waveDeliverySessionId,
                               entry.waveDeliveryGeneration,
                               entry.waveDeliveryBindingGeneration)) {
        static_cast<void>(host->close_world(entry.world));
        state::activity::release_binding(candidate.binding);
        report(core::log::Level::warn,
               "ev=mission stage=signal_scope result=fail activity=0x%016llX "
               "revision=%llu binding=%llu",
               static_cast<unsigned long long>(entry.waveDeliverySessionId),
               static_cast<unsigned long long>(entry.waveDeliveryGeneration),
               static_cast<unsigned long long>(entry.waveDeliveryBindingGeneration));
        return false;
    }
    table.bound[slot] = std::move(entry);
    g_activeWorlds.fetch_add(1, std::memory_order_release);
    report(core::log::Level::info,
           "ev=mission stage=compile result=ok destination=%s hash=0x%016llX "
           "mode=private_source",
           destination.c_str(),
           static_cast<unsigned long long>(compiled.program.hash));
    report(core::log::Level::info,
           "ev=physics stage=world result=opened session=0x%016llX activity=0x%016llX "
           "region=%d bubble=%u slice=%u peers=0 worlds=%zu mode=private_source",
           static_cast<unsigned long long>(candidate.binding.sessionId),
           static_cast<unsigned long long>(candidate.binding.sessionId),
           candidate.advertisedRegion,
           glimmer::kBubble,
           glimmer::kSlice,
           host->world_count());
    return true;
}

/**
 * Runs every tick one world owes, then reports the result at a bounded rate.
 * @param table Bridge table.
 * @param slot Occupied bridge slot.
 * @param now Monotonic tick count in milliseconds.
 */
void tick_bound(Storage& table, std::size_t slot, std::uint64_t now) noexcept {
    Bound& entry = table.bound[slot];
    BubbleHost* host = runtime::instance();
    if (host == nullptr || now < entry.nextTick) {
        return;
    }
    if (entry.privateSource) {
        server::bap::ActivitySnapshot current{};
        std::string destination;
        const bool exact = server::bap::snapshot_private_activity(current)
                           && current.bindingGeneration == entry.hostGeneration
                           && current.binding.sessionId == entry.retainedSource.sessionId
                           && current.binding.createdRevision
                                  == entry.retainedSource.createdRevision
                           && private_mission_source(current, destination);
        if (!exact) {
            close_bound(table, slot);
            table.privateAttempt = {};
            return;
        }
    }
    if (entry.missionPolicy != nullptr) {
        const std::uint64_t request = g_triggerRequested.load(std::memory_order_acquire);
        const std::uint64_t completed = g_triggerCompleted.load(std::memory_order_acquire);
        if (request != 0 && request != completed
            && entry.missionPolicy->queue_trigger_enter(
                g_triggerId.load(std::memory_order_acquire), request)) {
            g_triggerCompleted.store(request, std::memory_order_release);
            report(core::log::Level::info,
                   "ev=mission stage=trigger_inject result=accepted request=%llu trigger=0x%016llX",
                   static_cast<unsigned long long>(request),
                   static_cast<unsigned long long>(g_triggerId.load(std::memory_order_relaxed)));
        }
        signal_queue::Request signal{};
        if (signal_queue::peek(entry.waveDeliverySessionId,
                        entry.waveDeliveryGeneration,
                        entry.waveDeliveryBindingGeneration,
                        signal)) {
            if (!entry.missionPolicy->knows_content_signal(signal.id)) {
                static_cast<void>(signal_queue::consume(signal.sequence));
                report(core::log::Level::warn,
                       "ev=mission stage=content_signal result=rejected reason=unknown "
                       "request=%llu signal=0x%016llX scoped=%u activity=0x%016llX "
                       "revision=%llu binding=%llu",
                       static_cast<unsigned long long>(signal.sequence),
                       static_cast<unsigned long long>(signal.id),
                       static_cast<unsigned>(signal.scoped),
                       static_cast<unsigned long long>(signal.activitySessionId),
                       static_cast<unsigned long long>(signal.hostGeneration),
                       static_cast<unsigned long long>(signal.bindingGeneration));
            } else if (entry.missionPolicy->queue_content_signal(signal.id, signal.sequence)) {
                static_cast<void>(signal_queue::consume(signal.sequence));
                report(core::log::Level::info,
                       "ev=mission stage=content_signal result=accepted request=%llu "
                       "signal=0x%016llX scoped=%u activity=0x%016llX revision=%llu "
                       "binding=%llu",
                       static_cast<unsigned long long>(signal.sequence),
                       static_cast<unsigned long long>(signal.id),
                       static_cast<unsigned>(signal.scoped),
                       static_cast<unsigned long long>(signal.activitySessionId),
                       static_cast<unsigned long long>(signal.hostGeneration),
                       static_cast<unsigned long long>(signal.bindingGeneration));
            }
        }
    }
    // One tick per service slice. Catching a backlog up in one pass would run the world faster
    // than its fixed step for as long as the slice was late.
    entry.nextTick = now + kTickIntervalMs;
    const TickResult result = host->tick(entry.world);
    if (result.status != HostStatus::success) {
        report(core::log::Level::warn,
               "ev=physics stage=tick result=fail session=0x%016llX status=%u ticks=%llu",
               static_cast<unsigned long long>(entry.groupSessionId),
               static_cast<unsigned>(result.status),
               static_cast<unsigned long long>(entry.ticks));
        close_bound(table, slot);
        return;
    }
    ++entry.ticks;
    if (entry.missionPolicy != nullptr) {
        const mission::MissionState& state = entry.missionPolicy->state();
        g_programHash.store(entry.missionPolicy->program().hash, std::memory_order_release);
        g_missionState.store(state.missionState, std::memory_order_release);
        g_objective0.store(static_cast<std::uint32_t>(state.objectives[0]),
                           std::memory_order_release);
        std::uint32_t activated = 0;
        for (const bool value : state.activatedWaves) activated += value ? 1U : 0U;
        g_activatedWaveCount.store(activated, std::memory_order_release);
        mission::EnemyWaveIntent intent{};
        while (entry.missionPolicy->pop_enemy_wave_intent(intent)) {
            mission::EnemyWaveTicket ticket{};
            if (!mission::reserve_enemy_wave(entry.waveDeliverySessionId,
                                             entry.waveDeliveryGeneration,
                                             entry.waveDeliveryBindingGeneration,
                                             intent,
                                             ticket)) {
                report(core::log::Level::warn,
                       "ev=mission stage=wave_queue result=fail activity=0x%016llX command=%llu",
                       static_cast<unsigned long long>(entry.waveDeliverySessionId),
                       static_cast<unsigned long long>(intent.commandId));
                break;
            }
            report(core::log::Level::info,
                   "ev=mission stage=wave_queue result=reserved activity=0x%016llX command=%llu "
                   "wave=0x%016llX spawner=0x%08X ticket=%llu",
                   static_cast<unsigned long long>(entry.waveDeliverySessionId),
                   static_cast<unsigned long long>(intent.commandId),
                   static_cast<unsigned long long>(intent.waveId),
                   intent.spawnerDefinition,
                   static_cast<unsigned long long>(ticket.value));
        }
        mission::ContentStepIntent contentIntent{};
        while (entry.missionPolicy->peek_content_step_intent(contentIntent)) {
            mission::ContentStepTicket ticket{};
            if (!mission::reserve_content_step(entry.waveDeliverySessionId,
                                               entry.waveDeliveryGeneration,
                                               entry.waveDeliveryBindingGeneration,
                                               contentIntent,
                                               ticket)) {
                report(core::log::Level::warn,
                       "ev=mission stage=content_step_queue result=retry reason=backpressure "
                       "activity=0x%016llX revision=%llu binding=%llu command=%llu "
                       "step=0x%016llX kind=%u",
                       static_cast<unsigned long long>(entry.waveDeliverySessionId),
                       static_cast<unsigned long long>(entry.waveDeliveryGeneration),
                       static_cast<unsigned long long>(entry.waveDeliveryBindingGeneration),
                       static_cast<unsigned long long>(contentIntent.commandId),
                       static_cast<unsigned long long>(contentIntent.stepId),
                       static_cast<unsigned>(contentIntent.kind));
                break;
            }
            if (!entry.missionPolicy->consume_content_step_intent(contentIntent)) {
                const bool rolledBack = mission::cancel_content_step(ticket);
                report(core::log::Level::error,
                       "ev=mission stage=content_step_queue result=fail reason=head_changed "
                       "command=%llu step=0x%016llX ticket=%llu rollback=%u",
                       static_cast<unsigned long long>(contentIntent.commandId),
                       static_cast<unsigned long long>(contentIntent.stepId),
                       static_cast<unsigned long long>(ticket.value),
                       static_cast<unsigned>(rolledBack));
                break;
            }
            if (!mission::commit_content_step(ticket)) {
                report(core::log::Level::error,
                       "ev=mission stage=content_step_queue result=fail reason=commit_lost "
                       "command=%llu step=0x%016llX ticket=%llu",
                       static_cast<unsigned long long>(contentIntent.commandId),
                       static_cast<unsigned long long>(contentIntent.stepId),
                       static_cast<unsigned long long>(ticket.value));
                break;
            }
            report(core::log::Level::info,
                   "ev=mission stage=content_step_queue result=reserved "
                   "activity=0x%016llX revision=%llu binding=%llu command=%llu "
                   "step=0x%016llX kind=%u ticket=%llu",
                   static_cast<unsigned long long>(entry.waveDeliverySessionId),
                   static_cast<unsigned long long>(entry.waveDeliveryGeneration),
                   static_cast<unsigned long long>(entry.waveDeliveryBindingGeneration),
                   static_cast<unsigned long long>(contentIntent.commandId),
                   static_cast<unsigned long long>(contentIntent.stepId),
                   static_cast<unsigned>(contentIntent.kind),
                   static_cast<unsigned long long>(ticket.value));
        }
    }
    // One line a second. The tick itself is silent, so without this a running world and a stalled
    // one read the same.
    constexpr std::uint64_t kReportEveryTicks = 30;
    if (entry.ticks % kReportEveryTicks != 0) {
        return;
    }
    world::WorldSnapshot snapshot{};
    const HostStatus copied = host->snapshot(entry.world, snapshot);
    report(core::log::Level::debug,
           "ev=physics stage=tick result=ok session=0x%016llX activity=0x%016llX ticks=%llu "
           "stages=%zu actors=%zu peers=%zu",
           static_cast<unsigned long long>(entry.groupSessionId),
           static_cast<unsigned long long>(entry.activitySessionId),
           static_cast<unsigned long long>(entry.ticks),
           result.stageCount,
           copied == HostStatus::success ? snapshot.actorCount : 0U,
           entry.privateSource ? 0U : table.coordinators[slot].peer_count());
}

/**
 * Runs one bridge pass. Only the worker thread calls this.
 * @param now Monotonic tick count in milliseconds.
 * @param reconcile True to run the close and open passes as well as the ticks.
 */
void run_pass(std::uint64_t now, bool reconcile) noexcept {
    if (runtime::instance() == nullptr) {
        return;
    }
    Storage* table = storage();
    if (table == nullptr) {
        return;
    }
    const std::uint64_t reload = g_reloadRequested.load(std::memory_order_acquire);
    if (reload != g_reloadApplied) {
        // A trigger requested before the reload belongs to the retired policy. Mark that request
        // consumed without rewinding the monotonic request sequence. The id stays published so a
        // concurrent post-reload request cannot lose the id it stored before incrementing.
        const std::uint64_t triggerRequest =
            g_triggerRequested.load(std::memory_order_acquire);
        g_triggerCompleted.store(triggerRequest, std::memory_order_release);
        g_programHash.store(0, std::memory_order_release);
        g_missionState.store(0, std::memory_order_release);
        g_objective0.store(0, std::memory_order_release);
        g_activatedWaveCount.store(0, std::memory_order_release);
        for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
            if (table->bound[slot].occupied) close_bound(*table, slot);
        }
        table->attempts = {};
        table->privateAttempt = {};
        g_reloadApplied = reload;
        g_reloadCompleted.store(reload, std::memory_order_release);
        reconcile = true;
        report(core::log::Level::info,
               "ev=mission stage=reload result=accepted request=%llu",
               static_cast<unsigned long long>(reload));
    }
    if (!reconcile) {
        for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
            if (table->bound[slot].occupied) {
                tick_bound(*table, slot, now);
            }
        }
        return;
    }
    std::array<group::AdmittedRow, kAdmittedCapacity> admitted{};
    std::size_t admittedCount = 0;
    group::snapshot_admitted(admitted, admittedCount);
    std::array<group::HostSessionRow, kHostRowCapacity> rows{};
    std::size_t rowCount = 0;
    group::snapshot_host_sessions(rows, rowCount);
    server::bap::ActivitySnapshot privateSnapshot{};
    std::string privateDestination;
    const bool privateSourceAvailable =
        core::settings::get().server.activation.missionScriptHost
        && server::bap::snapshot_private_activity(privateSnapshot)
        && private_mission_source(privateSnapshot, privateDestination);
    if (!privateSourceAvailable) {
        table->authorizedPrivateSource = {};
    } else if (privateSnapshot.advertisedRegion
               == static_cast<std::int32_t>(
                   middleware::bap::activity_message::glimmer_extraction::kSlice)) {
        // Seeing the exact source in region 408 authorizes this BAP lifetime. The retail client
        // then reports region 24 during ordinary Trostland load-in, without changing the source.
        table->authorizedPrivateSource = privateSnapshot;
    } else if (table->authorizedPrivateSource.bindingGeneration == 0
               || !same_private_lifetime(table->authorizedPrivateSource, privateSnapshot)) {
        table->authorizedPrivateSource = {};
    }
    const bool privateAvailable =
        privateSourceAvailable && table->authorizedPrivateSource.bindingGeneration != 0
        && same_private_lifetime(table->authorizedPrivateSource, privateSnapshot);
    if (!privateAvailable) {
        table->privateAttempt = {};
    }

    // Closing runs first, so a peer that left this slice frees its world for the peer that
    // replaces it in the same slice.
    for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
        Bound& entry = table->bound[slot];
        if (!entry.occupied) {
            continue;
        }
        if (entry.privateSource) {
            const bool current =
                privateAvailable
                && privateSnapshot.bindingGeneration == entry.hostGeneration
                && privateSnapshot.binding.sessionId == entry.retainedSource.sessionId
                && privateSnapshot.binding.createdRevision == entry.retainedSource.createdRevision;
            if (!current) {
                close_bound(*table, slot);
                table->privateAttempt = {};
            }
            continue;
        }
        const group::HostSessionRow* row = find_host_row(rows, rowCount, entry.groupSessionId);
        // A rebound host row is a different activity, so the world it opened is finished even
        // though the group session id has not changed.
        const bool current = row != nullptr && row->generation == entry.hostGeneration
                             && row->hostSessionId == entry.activitySessionId;
        if (!current || !still_joined(admitted, admittedCount, entry.groupSessionId)) {
            const std::uint64_t closed = entry.groupSessionId;
            close_bound(*table, slot);
            clear_attempt(*table, closed);
        }
    }

    if (privateAvailable && find_private_bound(*table) == table->bound.size()) {
        std::size_t target = table->bound.size();
        for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
            if (!table->bound[slot].occupied) {
                target = slot;
                break;
            }
        }
        Attempt& attempt = table->privateAttempt;
        if (!attempt.occupied
            || attempt.groupSessionId != privateSnapshot.binding.sessionId
            || attempt.hostGeneration != privateSnapshot.bindingGeneration) {
            attempt = {privateSnapshot.binding.sessionId,
                       privateSnapshot.bindingGeneration,
                       0,
                       0,
                       true};
        }
        if (target != table->bound.size() && attempt.failures < kOpenAttemptLimit
            && now >= attempt.nextAttempt) {
            if (open_private_bound(*table, target, privateSnapshot, now)) {
                attempt = {};
            } else {
                ++attempt.failures;
                attempt.nextAttempt = now + kOpenRetryMs;
                if (attempt.failures == kOpenAttemptLimit) {
                    report(core::log::Level::warn,
                           "ev=physics stage=world result=abandoned session=0x%016llX "
                           "generation=%llu attempts=%u mode=private_source",
                           static_cast<unsigned long long>(privateSnapshot.binding.sessionId),
                           static_cast<unsigned long long>(privateSnapshot.bindingGeneration),
                           attempt.failures);
                }
            }
        }
    }

    for (std::size_t index = 0; index < admittedCount; ++index) {
        const group::AdmittedRow& peerRow = admitted[index];
        if (!peerRow.joinComplete || find_bound(*table, peerRow.sessionId) != table->bound.size()) {
            continue;
        }
        const group::HostSessionRow* row = find_host_row(rows, rowCount, peerRow.sessionId);
        if (row == nullptr) {
            continue;
        }
        std::size_t target = table->bound.size();
        for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
            if (!table->bound[slot].occupied) {
                target = slot;
                break;
            }
        }
        if (target == table->bound.size()) {
            continue;
        }
        Attempt* attempt = attempt_for(*table, peerRow.sessionId, row->generation);
        if (attempt == nullptr || attempt->failures >= kOpenAttemptLimit
            || now < attempt->nextAttempt) {
            continue;
        }
        if (open_bound(*table, target, peerRow, *row, now)) {
            clear_attempt(*table, peerRow.sessionId);
            continue;
        }
        ++attempt->failures;
        attempt->nextAttempt = now + kOpenRetryMs;
        if (attempt->failures == kOpenAttemptLimit) {
            report(core::log::Level::warn,
                   "ev=physics stage=world result=abandoned session=0x%016llX generation=%llu "
                   "attempts=%u",
                   static_cast<unsigned long long>(peerRow.sessionId),
                   static_cast<unsigned long long>(row->generation),
                   attempt->failures);
        }
    }

    for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
        if (table->bound[slot].occupied) {
            tick_bound(*table, slot, now);
        }
    }
}

/**
 * Owns the bridge for the whole time it runs.
 * Nothing else touches the table while this thread is alive, so the table needs no lock. The pump
 * only starts and stops it, and it joins before it reads anything.
 * @param parameter Unused.
 * @return Always zero.
 */
DWORD WINAPI worker_main(LPVOID parameter) noexcept {
    static_cast<void>(parameter);
    std::uint64_t nextReconcile = 0;
    for (;;) {
        const std::uint64_t now = GetTickCount64();
        const bool reconcile = now >= nextReconcile;
        if (reconcile) {
            nextReconcile = now + kReconcileIntervalMs;
        }
        run_pass(now, reconcile);
        if (WaitForSingleObject(g_stop, kWorkerSliceMs) == WAIT_OBJECT_0) {
            return 0;
        }
    }
}

/** Starts the worker if the gate is on and it is not already running. */
void start_worker() noexcept {
    if (g_thread != nullptr) {
        return;
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop == nullptr) {
        return;
    }
    g_thread = CreateThread(nullptr, 0, &worker_main, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        static_cast<void>(CloseHandle(g_stop));
        g_stop = nullptr;
        report(core::log::Level::warn, "ev=physics stage=worker result=fail reason=thread");
        return;
    }
    report(core::log::Level::info,
           "ev=physics stage=worker result=started slice=%llums",
           static_cast<unsigned long long>(kWorkerSliceMs));
}

/** Signals the worker and waits for it, so the caller owns the table when this returns. */
void stop_worker() noexcept {
    if (g_thread == nullptr) {
        return;
    }
    static_cast<void>(SetEvent(g_stop));
    static_cast<void>(WaitForSingleObject(g_thread, INFINITE));
    static_cast<void>(CloseHandle(g_thread));
    static_cast<void>(CloseHandle(g_stop));
    g_thread = nullptr;
    g_stop = nullptr;
    report(core::log::Level::info, "ev=physics stage=worker result=stopped");
}

} // namespace

/** Starts or stops the bridge worker. The bridge itself runs on that worker, not here. */
void service(std::uint64_t now) noexcept {
    static_cast<void>(now);
    // This runs on the callback pump, which is the game's own render thread. All this may cost it
    // is one settings read: the world ticks belong to the worker.
    if (!core::settings::get().server.activation.physicsHostSession) {
        reset();
        return;
    }
    start_worker();
}

std::uint64_t request_mission_reload() noexcept {
    return g_reloadRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
}

MissionReloadStatus mission_reload_status() noexcept {
    return {g_reloadRequested.load(std::memory_order_acquire),
            g_reloadCompleted.load(std::memory_order_acquire),
            g_activeWorlds.load(std::memory_order_acquire),
            g_triggerRequested.load(std::memory_order_acquire),
            g_triggerCompleted.load(std::memory_order_acquire),
            g_programHash.load(std::memory_order_acquire),
            g_missionState.load(std::memory_order_acquire),
            g_objective0.load(std::memory_order_acquire),
            g_activatedWaveCount.load(std::memory_order_acquire)};
}

std::uint64_t request_mission_trigger(std::uint64_t triggerId) noexcept {
    if (triggerId == 0) return 0;
    g_triggerId.store(triggerId, std::memory_order_release);
    return g_triggerRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool reserve_mission_content_signal(std::uint64_t activitySessionId,
                                    std::uint64_t hostGeneration,
                                    std::uint64_t bindingGeneration,
                                    std::uint64_t signalId,
                                    MissionContentSignalReservation& reservation) noexcept {
    reservation = {};
    reservation.sequence = signal_queue::reserve(activitySessionId,
                                                  hostGeneration,
                                                  bindingGeneration,
                                                  signalId,
                                                  true,
                                                  false);
    return reservation.sequence != 0;
}

bool commit_mission_content_signal(MissionContentSignalReservation reservation,
                                   MissionContentSignalCommit condition,
                                   void* context) noexcept {
    return signal_queue::commit_if(reservation.sequence, condition, context);
}

void abort_mission_content_signal(MissionContentSignalReservation reservation) noexcept {
    if (reservation.sequence != 0) {
        static_cast<void>(signal_queue::consume(reservation.sequence));
    }
}

std::uint64_t request_mission_content_signal(std::uint64_t signalId) noexcept {
    return signal_queue::reserve(0, 0, 0, signalId, false, true);
}

std::uint64_t request_mission_content_signal(std::uint64_t activitySessionId,
                                             std::uint64_t hostGeneration,
                                             std::uint64_t bindingGeneration,
                                             std::uint64_t signalId) noexcept {
    return signal_queue::reserve(activitySessionId,
                                 hostGeneration,
                                 bindingGeneration,
                                 signalId,
                                 true,
                                 true);
}

/** Stops the worker, then closes every open world and releases its State context. */
void reset() noexcept {
    // The join comes first. After it this thread is the only one that can reach the table.
    stop_worker();
    Storage* table = g_storage.get();
    if (table != nullptr) {
        for (std::size_t slot = 0; slot < table->bound.size(); ++slot) {
            if (table->bound[slot].occupied) {
                close_bound(*table, slot);
            }
        }
    }
    g_storage.reset();
    mission::reset_enemy_waves();
    mission::reset_content_steps();
    g_reloadRequested.store(0, std::memory_order_release);
    g_reloadCompleted.store(0, std::memory_order_release);
    g_triggerRequested.store(0, std::memory_order_release);
    g_triggerCompleted.store(0, std::memory_order_release);
    g_triggerId.store(0, std::memory_order_release);
    signal_queue::reset();
    g_programHash.store(0, std::memory_order_release);
    g_missionState.store(0, std::memory_order_release);
    g_objective0.store(0, std::memory_order_release);
    g_activatedWaveCount.store(0, std::memory_order_release);
    g_reloadApplied = 0;
    g_activeWorlds.store(0, std::memory_order_release);
}

} // namespace sunrise::server::gameplay::physics::host::session
