#include <array>
#include <cassert>
#include <cstring>
#include <memory>

#include "server/gameplay/mission/compiled_mission_policy.h"
#include "server/gameplay/mission/content_step_queue.h"
#include "server/gameplay/physics/world/world_runner.h"

namespace mission = sunrise::server::gameplay::mission;
namespace world = sunrise::server::gameplay::physics::world;

namespace {

class Executor final : public world::IHostCommandExecutor {
public:
    world::HostCommandExecutionResult execute_command(
        const world::HostExecutionContext& context,
        const world::HostCommand& command,
        std::span<const world::ActorSnapshot>,
        std::span<world::CommittedEvent> events) noexcept override {
        assert(world::command_kind(command) == world::HostCommandKind::createTrigger);
        assert(!events.empty());
        createdTrigger = std::get<world::CreateTriggerCommand>(command.payload);
        createTriggerSeen = true;
        events[0].commandId = command.header.commandId;
        events[0].tick = context.tick;
        events[0].value = createdTrigger.triggerId;
        events[0].kind = world::CommittedEventKind::triggerCreated;
        return {1, world::CommandRejectReason::none, world::HostCommandExecutionStatus::applied};
    }

    world::HostTickExecutionResult step_services(
        const world::HostExecutionContext& context,
        std::span<const world::ActorSnapshot>,
        std::span<const world::CommittedEvent>,
        std::span<world::CommittedEvent> events) noexcept override {
        world::HostTickExecutionResult result{};
        if (context.tick == 2) {
            assert(events.size() >= 2);
            events[0] = {};
            events[0].tick = context.tick;
            events[0].value = 22;
            events[0].actor = {44, 1};
            events[0].kind = world::CommittedEventKind::triggerEntered;
            events[1] = events[0];
            events[1].actor.generation = 2; // Distinct re-entry event in the same boundary.
            result.eventCount = 2;
        }
        return result;
    }

    world::CreateTriggerCommand createdTrigger{};
    bool createTriggerSeen{};
};

class Buffer final : public world::IPolicyStateWriter, public world::IPolicyStateReader {
public:
    bool write(std::span<const std::byte> input) noexcept override {
        if (input.size() > bytes.size()) return false;
        std::memcpy(bytes.data(), input.data(), input.size());
        size = input.size();
        return true;
    }
    bool read(std::span<std::byte> output) noexcept override {
        if (output.size() != size) return false;
        std::memcpy(output.data(), bytes.data(), size);
        return true;
    }
    std::array<std::byte, world::kPolicyRollbackCapacity> bytes{};
    std::size_t size{};
};

mission::MissionProgram program() {
    mission::MissionProgram value{};
    value.identity.bytes[0] = 'x'; value.identity.length = 1;
    value.destination.bytes[0] = 'd'; value.destination.length = 1;
    value.version = 1;
    value.budgets = {1, 1, 1, 1, 1, 0, 2};
    value.objectives[0].id = 11;
    value.objectives[0].initialState = mission::ObjectiveState::active;
    value.objectiveCount = 1;
    value.interactions[0].id = 22;
    value.interactions[0].bubble = 56;
    value.interactions[0].transform.position = {408.104538F, 406.008057F, 78.609955F};
    value.interactions[0].extents = {75.0F, 60.0F, 10.0F};
    value.interactionCount = 1;
    value.waves[0].id = 33;
    value.waves[0].spawnerDefinition = 0x80C26B0A;
    value.waves[0].requested = {1, 0};
    value.waves[0].requestedCount = 2;
    value.waveCount = 1;
    value.contentSteps[0].id = 55;
    value.contentSteps[0].kind = mission::ContentStepKind::glimmerIntro;
    value.contentStepCount = 1;
    value.transitions[0].event = mission::MissionEventKind::triggerEnter;
    value.transitions[0].source = 22;
    value.transitions[0].actions[0] = {mission::MissionActionKind::changeObjective, 11, 2};
    value.transitions[0].actions[1] = {mission::MissionActionKind::activateWave, 33, 0};
    value.transitions[0].actions[2] = {
        mission::MissionActionKind::activateContentStep, 55, 0};
    value.transitions[0].actions[3] = {
        mission::MissionActionKind::changeMissionState, 0, 1};
    value.transitions[0].actionCount = 4;
    value.contentSignals[0].id = 66;
    value.contentSignalCount = 1;
    value.transitions[1].event = mission::MissionEventKind::contentSignal;
    value.transitions[1].source = 66;
    value.transitions[1].requiredState = 1;
    value.transitions[1].actions[0] = {
        mission::MissionActionKind::changeMissionState, 0, 2};
    value.transitions[1].actionCount = 1;
    value.transitionCount = 2;
    value.hash = 0x1234;
    return value;
}

mission::MissionProgram dual_sequence_program() {
    mission::MissionProgram value{};
    value.identity.bytes[0] = 's'; value.identity.length = 1;
    value.destination.bytes[0] = 'd'; value.destination.length = 1;
    value.version = 1;
    value.budgets = {0, 0, 0, 2, 1, 0, 1};
    value.contentSteps[0].id = 101;
    value.contentSteps[0].kind = mission::ContentStepKind::glimmerIntro;
    value.contentSteps[1].id = 102;
    value.contentSteps[1].kind = mission::ContentStepKind::glimmerSite0Enter;
    value.contentStepCount = 2;
    value.contentSignals[0].id = 201;
    value.contentSignalCount = 1;
    value.transitions[0].event = mission::MissionEventKind::contentSignal;
    value.transitions[0].source = 201;
    value.transitions[0].actions[0] = {
        mission::MissionActionKind::activateContentStep, 101, 0};
    value.transitions[0].actions[1] = {
        mission::MissionActionKind::activateContentStep, 102, 0};
    value.transitions[0].actionCount = 2;
    value.transitionCount = 1;
    value.hash = 0x5678;
    return value;
}

} // namespace

int main() {
    mission::CompiledMissionPolicy policy;
    assert(!policy.configure(program(), 1));
    assert(policy.configure(program(), 86657));
    Executor executor;
    auto runner = std::make_unique<world::WorldRunner>();
    world::WorldOpenConfig open{};
    open.executor = &executor;
    open.activitySessionId = 1;
    open.ownerGeneration = 1;
    open.deterministicSeed = 7;
    assert(runner->open(open, &policy));
    assert(runner->advance(1).healthy);
    assert(executor.createTriggerSeen);
    assert(executor.createdTrigger.transform.position.x == 408.104538F);
    assert(executor.createdTrigger.transform.position.y == 406.008057F);
    assert(executor.createdTrigger.transform.position.z == 78.609955F);
    assert(executor.createdTrigger.transform.rotation.x == 0.0F);
    assert(executor.createdTrigger.transform.rotation.y == 0.0F);
    assert(executor.createdTrigger.transform.rotation.z == 0.0F);
    assert(executor.createdTrigger.transform.rotation.w == 1.0F);
    assert(executor.createdTrigger.halfExtents.x == 75.0F);
    assert(executor.createdTrigger.halfExtents.y == 60.0F);
    assert(executor.createdTrigger.halfExtents.z == 10.0F);
    assert(runner->advance(1).healthy);
    mission::EnemyWaveIntent intent{};
    assert(policy.pop_enemy_wave_intent(intent));
    assert(intent.spawnerDefinition == 0x80C26B0A && intent.requestedCount == 2);
    assert(policy.state().objectives[0] == mission::ObjectiveState::complete);
    assert(policy.state().activatedWaves[0]);
    mission::ContentStepIntent contentIntent{};
    assert(policy.peek_content_step_intent(contentIntent));
    assert(contentIntent.kind == mission::ContentStepKind::glimmerIntro);
    mission::ContentStepIntent changedHead = contentIntent;
    ++changedHead.commandId;
    assert(!policy.consume_content_step_intent(changedHead));
    mission::ContentStepIntent retainedIntent{};
    assert(policy.peek_content_step_intent(retainedIntent));
    assert(retainedIntent.commandId == contentIntent.commandId
           && retainedIntent.stepId == contentIntent.stepId
           && retainedIntent.kind == contentIntent.kind);
    assert(policy.consume_content_step_intent(contentIntent));
    assert(!policy.peek_content_step_intent(retainedIntent));
    assert(policy.state().activatedContentSteps[0]);
    assert(policy.state().missionState == 1);
    assert(!policy.knows_content_signal(99));
    assert(policy.knows_content_signal(66));
    assert(!policy.queue_content_signal(99, 1));
    assert(policy.queue_content_signal(66, 1));
    assert(runner->advance(1).healthy);
    assert(policy.state().missionState == 2);
    assert(policy.queue_content_signal(66, 2));
    assert(runner->advance(1).healthy);
    assert(policy.state().missionState == 2); // duplicate/out-of-order state is ignored
    assert(!policy.pop_content_step_intent(contentIntent));
    assert(!policy.pop_enemy_wave_intent(intent));
    assert(runner->advance(1).healthy);
    assert(!policy.pop_enemy_wave_intent(intent));

    Buffer buffer;
    assert(policy.save(buffer));
    mission::CompiledMissionPolicy restored;
    assert(restored.configure(program(), 86657));
    assert(restored.load(buffer));
    assert(restored.state().objectives[0] == mission::ObjectiveState::complete);
    assert(restored.state().missionState == 14); // active restart enters cleanup/cooldown

    mission::CompiledMissionPolicy injected;
    assert(injected.configure(program(), 86657));
    auto injectedRunner = std::make_unique<world::WorldRunner>();
    assert(injectedRunner->open(open, &injected));
    assert(!injected.queue_trigger_enter(99, 1));
    assert(injected.queue_trigger_enter(22, 1));
    assert(!injected.queue_trigger_enter(22, 2));
    assert(injectedRunner->advance(1).healthy);
    assert(injected.pop_enemy_wave_intent(intent));
    assert(injected.state().objectives[0] == mission::ObjectiveState::complete);
    assert(injected.state().missionState == 1);

    mission::CompiledMissionPolicy dualSequence;
    assert(dualSequence.configure(dual_sequence_program(), 86657));
    auto dualSequenceRunner = std::make_unique<world::WorldRunner>();
    assert(dualSequenceRunner->open(open, &dualSequence));
    assert(dualSequence.queue_content_signal(201, 1));
    assert(dualSequenceRunner->advance(1).healthy);
    mission::ContentStepIntent introIntent{};
    mission::ContentStepIntent enterIntent{};
    assert(dualSequence.peek_content_step_intent(introIntent));
    assert(introIntent.kind == mission::ContentStepKind::glimmerIntro);

    // A full downstream queue leaves the exact policy FIFO head untouched. Once one provisional
    // slot is released, reserve + exact consume + commit admits that same command in order.
    mission::reset_content_steps();
    std::array<mission::ContentStepTicket, 32> fullTickets{};
    mission::ContentStepIntent filler{};
    filler.kind = mission::ContentStepKind::glimmerIntro;
    for (std::size_t index = 0; index < fullTickets.size(); ++index) {
        filler.commandId = 10'000 + index;
        filler.stepId = 20'000 + index;
        assert(mission::reserve_content_step(500, 600, 700, filler, fullTickets[index]));
    }
    mission::ContentStepTicket admitted{};
    assert(!mission::reserve_content_step(500, 600, 700, introIntent, admitted));
    mission::ContentStepIntent retainedHead{};
    assert(dualSequence.peek_content_step_intent(retainedHead));
    assert(retainedHead.commandId == introIntent.commandId
           && retainedHead.stepId == introIntent.stepId
           && retainedHead.kind == introIntent.kind);
    assert(mission::cancel_content_step(fullTickets[0]));
    assert(mission::reserve_content_step(500, 600, 700, introIntent, admitted));
    assert(dualSequence.consume_content_step_intent(introIntent));
    assert(mission::commit_content_step(admitted));
    mission::QueuedContentStep admittedStep{};
    assert(mission::peek_content_step(500, 600, 700, admittedStep));
    assert(admittedStep.ticket.value == admitted.value
           && admittedStep.intent.commandId == introIntent.commandId);
    assert(dualSequence.pop_content_step_intent(enterIntent));
    assert(enterIntent.kind == mission::ContentStepKind::glimmerSite0Enter);
    assert(introIntent.commandId < enterIntent.commandId);
    mission::cancel_content_steps(500, 600, 700);
    assert(dualSequence.state().activatedContentSteps[0]);
    assert(dualSequence.state().activatedContentSteps[1]);
    assert(!dualSequence.pop_content_step_intent(contentIntent));
}
