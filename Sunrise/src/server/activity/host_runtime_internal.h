#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "host_runtime.h"

namespace sunrise::server::activity::host::detail {

/** One queued operator transition. */
struct ControlRequest final {
    state::activity::SessionBinding binding{};
    std::uint8_t lifetimeState{kDefaultLifetimeState};
};

/** One queued operator incident. */
struct IncidentRequest final {
    state::activity::SessionBinding binding{};
    middleware::bap::activity_message::incident::Incident incident{};
};

/** One queued typed ClientRef request; its counter is assigned by the reducer. */
struct ScriptableRequest final {
    state::activity::SessionBinding binding{};
    ScriptableTarget target{};
    state::build_data::scenarios::RosterGroup stateLocalRosterGroup{};
    std::array<std::int32_t,
               middleware::bap::activity_message::squad_auth::kMaximumRequestedCountLength>
        requestedCounts{};
    std::array<std::int8_t, 4> squadAuthoredProfile{};
    std::array<std::byte,
               middleware::bap::activity_message::sensor_auth_update::kAuthOverrideByteCapacity>
        authBody{};
    std::size_t requestedCountLength{};
    std::uint16_t authBitCount{};
    std::uint16_t authByteCount{};
    std::uint16_t dialogueCue{};
    std::optional<std::uint32_t> nameHash{};
    std::uint64_t expectedActivityClientGeneration{};
    /** Exact reserved revision, or zero for an ordinary operator request. */
    std::uint64_t expectedRevision{};
    /** Exact durable Mission State head, or zero for an ordinary operator request. */
    std::uint64_t expectedIntentSequence{};
    /** This body shares the head's reserved revision instead of holding one of its own. */
    bool burstMember{};
    middleware::bap::activity_message::scriptable_auth::Type23Channel channel{};
    float value{};
    std::uint32_t channelHash{};
    std::int32_t entryIndex{};
    middleware::bap::activity_message::squad_auth::Mode squadMode{
        middleware::bap::activity_message::squad_auth::Mode::mode0};
    ScriptableOverrideKind kind{ScriptableOverrideKind::type23};
    /** Activity lifetime state for a lifetime request; ignored by every other kind. */
    std::uint8_t lifetimeState{kDefaultLifetimeState};
    bool snap{};
    bool active{};
};

/** One owned client envelope without a richer typed mission reducer. */
struct ClientMessageMissionInput final {
    state::activity::SessionBinding binding{};
    std::uint64_t sourceGeneration{};
    std::uint64_t clientMessageSequence{};
    std::uint32_t messageType{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    std::uint32_t consumedBits{};
    ClientMessageStatus status{ClientMessageStatus::unclassified};
};

/** Input kind retained in the one ordered reducer queue. */
enum class PendingKind : std::uint8_t {
    sense,
    incident,
    clientStateChange,
    entitySlotsRequested,
    clientMessage,
    authControl,
    incidentControl,
    scriptableControl,
    /** Exact queued durable control withdrawn under the Host lock; accounting is already paid. */
    discardedControl,
};

/** One copied input; only the field selected by kind is applied. */
struct PendingInput final {
    SenseInput sense{};
    IncidentInput incident{};
    ClientStateChangeInput clientStateChange{};
    EntitySlotsRequestedInput entitySlotsRequested{};
    ClientMessageMissionInput clientMessage{};
    ControlRequest control{};
    IncidentRequest incidentControl{};
    ScriptableRequest scriptableControl{};
    PendingKind kind{PendingKind::sense};
};

/** Committed monotonic guards for one full ClientRef identity. */
struct ScriptableGuard final {
    ScriptableTarget target{};
    middleware::bap::activity_message::squad_auth::GenerationGuard squad{};
    middleware::bap::activity_message::scriptable_auth::Type2ChannelState type2{};
    middleware::bap::activity_message::scriptable_auth::Type4GenerationGuard type4{};
    middleware::bap::activity_message::scriptable_auth::Type5RevisionGuard type5{};
    middleware::bap::activity_message::scriptable_auth::Type6GenerationGuard type6{};
    middleware::bap::activity_message::scriptable_auth::Type23SequenceGuard type23{};
    middleware::bap::activity_message::scriptable_auth::Type31GenerationGuard type31{};
    middleware::bap::activity_message::scriptable_auth::Type3GenerationGuard type3{};
    middleware::bap::activity_message::scriptable_auth::Type38GenerationGuard type38{};
    middleware::bap::activity_message::scriptable_auth::Type53SequenceGuard type53{};
    middleware::bap::activity_message::scriptable_auth::Type42GenerationGuard type42{};
    std::uint32_t authoredSceneGeneration{};
    bool occupied{};
};

/** One change guard for a decoded type-43 Sense object. */
struct SceneSenseTraceRecord final {
    SenseObservationKey key{};
    std::uint64_t fingerprint{};
    std::uint32_t generationPlusOne{};
    std::uint32_t valueCount{};
    bool hasGeneration{};
    bool occupied{};
};

/** Bounded change guards for one ActivityClient generation. */
struct SceneSenseTrace final {
    std::array<SceneSenseTraceRecord,
               middleware::bap::activity_message::sense_update::kDecodedObjectCapacity>
        records{};
    std::uint64_t sourceGeneration{};
    bool incompleteReported{};
    bool capacityReported{};
};

/** Mutable per-instance storage kept behind the runtime lock. */
struct Instance final {
    InstanceSnapshot view{};
    SenseObservationSnapshot senseObservations{};
    SceneSenseTrace sceneSenseTrace{};
    std::array<ScriptableGuard, kScriptableGuardCapacity> scriptableGuards{};
    /** Latest delivered body for every full ClientRef, re-emitted by every later msg-5 body. */
    std::vector<PendingScriptableOverride> scriptableAuthEstate{};
    PendingScriptableOverride pendingScriptable{};
    /** Committed bodies waiting on the same push as the head. Never a squad or a lifetime. */
    std::array<PendingScriptableOverride, kPendingScriptableTailCapacity> pendingScriptableTail{};
    std::size_t pendingScriptableTailCount{};
    ScriptableOutputReservation scriptableReservation{};
    std::uint64_t lastTouched{};
    std::uint64_t missionSequence{};
    bool occupied{};
};

/** Clears one instance member by member; a whole-value assignment puts 1 MiB on the stack. */
inline void clear_instance(Instance& instance) noexcept {
    instance.view = {};
    instance.senseObservations = {};
    instance.sceneSenseTrace = {};
    instance.scriptableGuards.fill({});
    std::vector<PendingScriptableOverride>{}.swap(instance.scriptableAuthEstate);
    instance.pendingScriptable = {};
    instance.pendingScriptableTail.fill({});
    instance.pendingScriptableTailCount = 0;
    instance.scriptableReservation = {};
    instance.lastTouched = 0;
    instance.missionSequence = 0;
    instance.occupied = false;
}

extern SRWLOCK g_lock;
extern std::array<Instance, kInstanceCapacity> g_instances;
/** Ordered reducer work grows with real input and fails only when allocation fails. */
extern std::vector<PendingInput> g_pending;
extern std::size_t g_pendingRead;
extern std::size_t g_queuedControls;
extern std::uint64_t g_refusedControls;
extern std::uint64_t g_sequence;
extern std::uint64_t g_scriptableReservationGeneration;
extern std::uint64_t g_scriptableReservationSequence;

/** Finds one exact instance while the runtime lock is held. */
[[nodiscard]] Instance* find_instance(const state::activity::SessionBinding& binding) noexcept;

/** @return True when this exact binding already has an operator request waiting to reduce. */
[[nodiscard]] bool has_queued_control(const state::activity::SessionBinding& binding) noexcept;

/** Appends one owned reducer row while the runtime lock is held. */
[[nodiscard]] bool append_pending(PendingInput&& pending) noexcept;

/** Moves one instance to the newest eviction position. */
void touch(Instance& instance) noexcept;

/** Appends one event in oldest-to-newest ring order. */
void append_event(Event& event) noexcept;

/** Applies one typed request from the shared reducer queue. */
void apply_scriptable_control(const ScriptableRequest& request, std::uint64_t now) noexcept;

} // namespace sunrise::server::activity::host::detail
