#include "compiled_mission_policy.h"

#include <cstring>
#include <limits>
#include <string_view>

#include "../../../middleware/bap/activity_message/glimmer_extraction_contract.h"

namespace sunrise::server::gameplay::mission {
namespace {

constexpr std::uint64_t kPolicyId = 0x4D495353494F4E01ULL;
constexpr std::uint64_t kPolicyOwnerId = 0x4D495353494F4E02ULL;
constexpr physics::world::BlueprintRef kTriggerProfile{0x4D49535354524701ULL, 1};

[[nodiscard]] std::uint64_t event_id(const physics::world::CommittedEvent& event) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    const std::uint64_t fields[] = {event.tick,
                                    static_cast<std::uint64_t>(event.kind),
                                    event.value,
                                    event.actor.id,
                                    event.actor.generation};
    for (const std::uint64_t field : fields) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value = (value ^ ((field >> shift) & 0xFFU)) * 1099511628211ULL;
        }
    }
    return value == 0 ? 1 : value;
}

[[nodiscard]] MissionEventKind mission_event_kind(
    physics::world::CommittedEventKind kind) noexcept {
    switch (kind) {
    case physics::world::CommittedEventKind::triggerEntered:
        return MissionEventKind::triggerEnter;
    case physics::world::CommittedEventKind::triggerStayed:
        return MissionEventKind::triggerStay;
    case physics::world::CommittedEventKind::triggerLeft:
        return MissionEventKind::triggerLeave;
    case physics::world::CommittedEventKind::timerFired:
        return MissionEventKind::timerFired;
    default:
        return MissionEventKind::count;
    }
}

} // namespace

bool CompiledMissionPolicy::configure(const MissionProgram& program,
                                      std::uint64_t contentBuildId) noexcept {
    if (configured_ || program.hash == 0 || program.version == 0 || contentBuildId == 0
        || program.interactionCount > program.budgets.triggers
        || program.objectiveCount > program.budgets.objectives
        || program.waveCount > program.budgets.waves
        || program.contentStepCount > program.budgets.contentSteps
        || program.transitionCount > program.budgets.transitions
        || (program.contentStepCount != 0
            && contentBuildId
                   != middleware::bap::activity_message::glimmer_extraction::kBuildId)) {
        return false;
    }
    program_ = program;
    manifest_.policy = {kPolicyId ^ program.hash, program.version};
    if (manifest_.policy.id == 0) manifest_.policy.id = kPolicyId;
    manifest_.contentBuildId = contentBuildId;
    manifest_.allowedCommandMask = physics::world::command_kind_mask(
        physics::world::HostCommandKind::createTrigger)
        | physics::world::command_kind_mask(physics::world::HostCommandKind::emitMappedIncident)
        | physics::world::command_kind_mask(physics::world::HostCommandKind::scheduleTickTimer);
    manifest_.fixedRateHz = physics::world::kDefaultFixedRateHz;
    manifest_.triggerBudget = program.budgets.triggers;
    manifest_.counterBudget = program.budgets.objectives;
    manifest_.persistenceSchemaVersion = 1;
    configured_ = true;
    return true;
}

physics::world::ActivityPolicyManifest CompiledMissionPolicy::manifest() const noexcept {
    return configured_ ? manifest_ : physics::world::ActivityPolicyManifest{};
}

physics::world::PolicyCommandMeta CompiledMissionPolicy::command_meta(
    physics::world::TickId tick) noexcept {
    physics::world::PolicyCommandMeta meta{};
    meta.definition = manifest_.policy;
    meta.commandId = state_.nextCommandId++;
    meta.executeTick = tick;
    meta.policyOwnerId = kPolicyOwnerId;
    return meta;
}

bool CompiledMissionPolicy::initialize(const physics::world::ActivityPolicyContext& context,
                                       physics::world::HostCommands& commands) noexcept {
    if (!configured_ || initialized_ || context.fixedRateHz != manifest_.fixedRateHz) return false;
    state_ = {};
    state_.schemaVersion = manifest_.persistenceSchemaVersion;
    state_.nextCommandId = 1;
    for (std::size_t index = 0; index < program_.objectiveCount; ++index) {
        state_.objectives[index] = program_.objectives[index].initialState;
    }
    for (std::size_t index = 0; index < program_.interactionCount; ++index) {
        const ProximityInteraction& interaction = program_.interactions[index];
        physics::world::CreateTriggerCommand trigger{};
        trigger.triggerId = interaction.id;
        trigger.transform = interaction.transform;
        trigger.halfExtents = interaction.extents;
        trigger.triggerProfile = kTriggerProfile;
        if (commands.create_trigger(command_meta(1), trigger)
            != physics::world::CommandSubmitStatus::accepted) {
            return false;
        }
    }
    initialized_ = true;
    MissionEvent event{};
    event.kind = MissionEventKind::initialized;
    event.source = program_.identity.length == 0 ? 0 : [&]() noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t i = 0; i < program_.identity.length; ++i)
            hash = (hash ^ static_cast<unsigned char>(program_.identity.bytes[i])) * 1099511628211ULL;
        return hash;
    }();
    event.eventId = event.source;
    event.tick = 0;
    return process_event(event, commands);
}

void CompiledMissionPolicy::pre_tick(const physics::world::PolicyTickContext& context,
                                     physics::world::HostCommands& commands) noexcept {
    if (restartCooldownTimerPending_) {
        restartCooldownTimerPending_ = false;
        for (std::size_t index = 0; index < program_.timerCount; ++index) {
            if (std::string_view(program_.timers[index].name.bytes.data(),
                                 program_.timers[index].name.length) != "cooldown") continue;
            physics::world::ScheduleTickTimerCommand timer{};
            timer.timerId = program_.timers[index].id;
            timer.fireTick = context.tick + program_.timers[index].delayTicks;
            timer.timerGeneration = state_.nextCommandId;
            (void)commands.schedule_tick_timer(command_meta(context.tick), timer);
            break;
        }
    }
    if (!researchEventPending_) return;
    MissionEvent event = pendingResearchEvent_;
    event.tick = context.tick;
    researchEventPending_ = false;
    pendingResearchEvent_ = {};
    (void)process_event(event, commands);
}

void CompiledMissionPolicy::post_tick(
    const physics::world::PolicyTickContext& context,
    std::span<const physics::world::CommittedEvent> committedEvents,
    physics::world::HostCommands& commands) noexcept {
    for (const physics::world::CommittedEvent& committed : committedEvents) {
        const MissionEventKind kind = mission_event_kind(committed.kind);
        if (kind == MissionEventKind::count) continue;
        MissionEvent event{};
        event.kind = kind;
        event.source = committed.value;
        event.eventId = event_id(committed);
        event.tick = context.tick;
        (void)process_event(event, commands);
    }
}

bool CompiledMissionPolicy::process_event(const MissionEvent& event,
                                          physics::world::HostCommands& commands) noexcept {
    for (std::size_t index = 0; index < state_.processedEventCount; ++index) {
        if (state_.processedEvents[index] == event.eventId) return true;
    }
    for (std::size_t index = 0; index < program_.transitionCount; ++index) {
        const MissionTransition& transition = program_.transitions[index];
        if (transition.event != event.kind || transition.source != event.source) continue;
        if (transition.requiredState != (std::numeric_limits<std::uint32_t>::max)()
            && transition.requiredState != state_.missionState) continue;
        for (std::size_t action = 0; action < transition.actionCount; ++action) {
            if (!process_action(transition.actions[action], event.tick, commands)) return false;
        }
    }
    if (state_.processedEventCount == state_.processedEvents.size()) {
        for (std::size_t index = 1; index < state_.processedEvents.size(); ++index) {
            state_.processedEvents[index - 1] = state_.processedEvents[index];
        }
        --state_.processedEventCount;
    }
    state_.processedEvents[state_.processedEventCount++] = event.eventId;
    state_.lastEventId = event.eventId;
    return true;
}

bool CompiledMissionPolicy::process_action(const MissionAction& action,
                                           physics::world::TickId tick,
                                           physics::world::HostCommands& commands) noexcept {
    if (action.kind == MissionActionKind::activateWave) {
        for (std::size_t index = 0; index < program_.waveCount; ++index) {
            if (program_.waves[index].id != action.target) continue;
            if (state_.activatedWaves[index]) return true;
            if (intentCount_ == intents_.size()) return false;
            const EnemyWave& wave = program_.waves[index];
            EnemyWaveIntent& intent = intents_[(intentHead_ + intentCount_) % intents_.size()];
            intent.commandId = state_.nextCommandId++;
            intent.waveId = wave.id;
            intent.spawnerDefinition = wave.spawnerDefinition;
            intent.mode = wave.mode;
            intent.requested = wave.requested;
            intent.requestedCount = wave.requestedCount;
            ++intentCount_;
            state_.activatedWaves[index] = true;
            return true;
        }
        return false;
    }
    if (action.kind == MissionActionKind::activateContentStep) {
        for (std::size_t index = 0; index < program_.contentStepCount; ++index) {
            if (program_.contentSteps[index].id != action.target) continue;
            if (state_.activatedContentSteps[index]) return true;
            if (contentIntentCount_ == contentIntents_.size()) return false;
            ContentStepIntent& intent = contentIntents_[
                (contentIntentHead_ + contentIntentCount_) % contentIntents_.size()];
            intent.commandId = state_.nextCommandId++;
            intent.stepId = program_.contentSteps[index].id;
            intent.kind = program_.contentSteps[index].kind;
            ++contentIntentCount_;
            state_.activatedContentSteps[index] = true;
            return true;
        }
        return false;
    }
    if (action.kind == MissionActionKind::changeObjective) {
        for (std::size_t index = 0; index < program_.objectiveCount; ++index) {
            if (program_.objectives[index].id == action.target) {
                state_.objectives[index] = static_cast<ObjectiveState>(action.value);
                return true;
            }
        }
        return false;
    }
    if (action.kind == MissionActionKind::changeMissionState) {
        state_.missionState = static_cast<std::uint32_t>(action.value);
        if (state_.missionState == 0) {
            state_.activatedWaves = {};
            state_.activatedContentSteps = {};
        }
        return true;
    }
    if (action.kind == MissionActionKind::scheduleTimer) {
        for (std::size_t index = 0; index < program_.timerCount; ++index) {
            if (program_.timers[index].id != action.target) continue;
            physics::world::ScheduleTickTimerCommand timer{};
            timer.timerId = action.target;
            timer.fireTick = tick + program_.timers[index].delayTicks;
            timer.timerGeneration = state_.nextCommandId;
            const auto status = commands.schedule_tick_timer(command_meta(tick + 1), timer);
            return status == physics::world::CommandSubmitStatus::accepted
                   || status == physics::world::CommandSubmitStatus::duplicate;
        }
        return false;
    }
    if (action.kind == MissionActionKind::emitIncident) {
        physics::world::EmitMappedIncidentCommand incident{};
        incident.incidentId = action.value;
        const physics::world::CommandSubmitStatus status =
            commands.emit_mapped_incident(command_meta(tick + 1), incident);
        return status == physics::world::CommandSubmitStatus::accepted
               || status == physics::world::CommandSubmitStatus::duplicate;
    }
    return false;
}

bool CompiledMissionPolicy::save(physics::world::IPolicyStateWriter& writer) const noexcept {
    Persisted persisted{};
    persisted.programHash = program_.hash;
    persisted.state = state_;
    persisted.intents = intents_;
    persisted.intentHead = intentHead_;
    persisted.intentCount = intentCount_;
    persisted.contentIntents = contentIntents_;
    persisted.contentIntentHead = contentIntentHead_;
    persisted.contentIntentCount = contentIntentCount_;
    return writer.write({reinterpret_cast<const std::byte*>(&persisted), sizeof(persisted)});
}

bool CompiledMissionPolicy::load(physics::world::IPolicyStateReader& reader) noexcept {
    if (!configured_) return false;
    Persisted persisted{};
    if (!reader.read({reinterpret_cast<std::byte*>(&persisted), sizeof(persisted)})
        || persisted.programHash != program_.hash
        || persisted.state.schemaVersion != manifest_.persistenceSchemaVersion
        || persisted.intentHead >= intents_.size() || persisted.intentCount > intents_.size()
        || persisted.contentIntentHead >= contentIntents_.size()
        || persisted.contentIntentCount > contentIntents_.size()) {
        return false;
    }
    state_ = persisted.state;
    intents_ = persisted.intents;
    intentHead_ = persisted.intentHead;
    intentCount_ = persisted.intentCount;
    contentIntents_ = persisted.contentIntents;
    contentIntentHead_ = persisted.contentIntentHead;
    contentIntentCount_ = persisted.contentIntentCount;
    if (state_.missionState > 0 && state_.missionState < 14) {
        const ContentStepKind cleanup = state_.missionState <= 4
                                            ? ContentStepKind::glimmerSite0Exit
                                            : state_.missionState <= 8
                                                  ? ContentStepKind::glimmerSite1Exit
                                                  : ContentStepKind::glimmerSite2Exit;
        intents_ = {};
        intentHead_ = 0;
        intentCount_ = 0;
        contentIntents_ = {};
        contentIntentHead_ = 0;
        for (std::size_t index = 0; index < program_.contentStepCount; ++index) {
            if (program_.contentSteps[index].kind != cleanup) continue;
            contentIntentCount_ = 1;
            contentIntents_[0] = {
                state_.nextCommandId++, program_.contentSteps[index].id, cleanup};
            break;
        }
        state_.missionState = 14;
        restartCooldownTimerPending_ = true;
    } else if (state_.missionState == 14) {
        restartCooldownTimerPending_ = true;
    }
    initialized_ = true;
    return true;
}

bool CompiledMissionPolicy::peek_content_step_intent(ContentStepIntent& intent) const noexcept {
    intent = {};
    if (contentIntentCount_ == 0) return false;
    intent = contentIntents_[contentIntentHead_];
    return true;
}

bool CompiledMissionPolicy::consume_content_step_intent(
    const ContentStepIntent& expected) noexcept {
    if (contentIntentCount_ == 0) return false;
    const ContentStepIntent& head = contentIntents_[contentIntentHead_];
    if (head.commandId != expected.commandId || head.stepId != expected.stepId
        || head.kind != expected.kind) return false;
    contentIntents_[contentIntentHead_] = {};
    contentIntentHead_ = static_cast<std::uint8_t>(
        (contentIntentHead_ + 1) % contentIntents_.size());
    --contentIntentCount_;
    return true;
}

bool CompiledMissionPolicy::pop_content_step_intent(ContentStepIntent& intent) noexcept {
    if (!peek_content_step_intent(intent)) return false;
    return consume_content_step_intent(intent);
}

bool CompiledMissionPolicy::pop_enemy_wave_intent(EnemyWaveIntent& intent) noexcept {
    intent = {};
    if (intentCount_ == 0) return false;
    intent = intents_[intentHead_];
    intents_[intentHead_] = {};
    intentHead_ = static_cast<std::uint8_t>((intentHead_ + 1) % intents_.size());
    --intentCount_;
    return true;
}

bool CompiledMissionPolicy::queue_trigger_enter(std::uint64_t triggerId,
                                                std::uint64_t requestId) noexcept {
    if (!initialized_ || researchEventPending_ || triggerId == 0 || requestId == 0
        || requestId > ((std::numeric_limits<std::uint64_t>::max)() >> 2U)) return false;
    bool known = false;
    for (std::size_t index = 0; index < program_.interactionCount; ++index) {
        known = known || program_.interactions[index].id == triggerId;
    }
    if (!known) return false;
    pendingResearchEvent_.kind = MissionEventKind::triggerEnter;
    pendingResearchEvent_.source = triggerId;
    pendingResearchEvent_.eventId = (requestId << 2U) | 1U;
    researchEventPending_ = true;
    return true;
}

bool CompiledMissionPolicy::knows_content_signal(std::uint64_t signalId) const noexcept {
    if (signalId == 0) return false;
    for (std::size_t index = 0; index < program_.contentSignalCount; ++index) {
        if (program_.contentSignals[index].id == signalId) return true;
    }
    return false;
}

bool CompiledMissionPolicy::queue_content_signal(std::uint64_t signalId,
                                                 std::uint64_t observationId) noexcept {
    if (!initialized_ || researchEventPending_ || observationId == 0
        || observationId > ((std::numeric_limits<std::uint64_t>::max)() >> 2U)
        || !knows_content_signal(signalId)) {
        return false;
    }
    pendingResearchEvent_.kind = MissionEventKind::contentSignal;
    pendingResearchEvent_.source = signalId;
    pendingResearchEvent_.eventId = (observationId << 2U) | 2U;
    researchEventPending_ = true;
    return true;
}

} // namespace sunrise::server::gameplay::mission
