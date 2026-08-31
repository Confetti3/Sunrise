#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../physics/world/world_types.h"

namespace sunrise::server::gameplay::mission {

inline constexpr std::size_t kMissionTextCapacity = 64;
inline constexpr std::size_t kObjectiveCapacity = 16;
inline constexpr std::size_t kInteractionCapacity = 16;
inline constexpr std::size_t kEnemyWaveCapacity = 16;
inline constexpr std::size_t kContentStepCapacity = 16;
inline constexpr std::size_t kContentSignalCapacity = 16;
inline constexpr std::size_t kTimerCapacity = 16;
inline constexpr std::size_t kTransitionCapacity = 32;
inline constexpr std::size_t kActionsPerTransition = 4;
inline constexpr std::size_t kRequestedSlotCapacity = 8;
inline constexpr std::size_t kEnemyWaveIntentCapacity = 16;
inline constexpr std::size_t kContentStepIntentCapacity = 16;

struct MissionText final {
    std::array<char, kMissionTextCapacity> bytes{};
    std::uint8_t length{};
};

enum class ObjectiveState : std::uint8_t { inactive, active, complete, count };
enum class MissionEventKind : std::uint8_t {
    initialized,
    triggerEnter,
    triggerStay,
    triggerLeave,
    contentSignal,
    timerFired,
    count,
};
enum class MissionActionKind : std::uint8_t {
    activateWave,
    activateContentStep,
    scheduleTimer,
    changeObjective,
    emitIncident,
    changeMissionState,
    count,
};

struct MissionBudgets final {
    std::uint32_t triggers{};
    std::uint32_t objectives{};
    std::uint32_t waves{};
    std::uint32_t contentSteps{};
    std::uint32_t contentSignals{};
    std::uint32_t timers{};
    std::uint32_t transitions{};
};

enum class ContentStepKind : std::uint8_t {
    glimmerIntro,
    glimmerSite0ShipSpawn,
    glimmerSite0Enter,
    glimmerSite0Crew,
    glimmerSite0Exit,
    glimmerSite1ShipSpawn,
    glimmerSite1Enter,
    glimmerSite1Crew,
    glimmerSite1Exit,
    glimmerSite2ShipSpawn,
    glimmerSite2Enter,
    glimmerSite2Crew,
    glimmerSite2Exit,
    glimmerComplete,
    glimmerNormalChest,
    glimmerCleanup,
    count,
};

struct ContentStep final {
    std::uint64_t id{};
    MissionText name{};
    ContentStepKind kind{ContentStepKind::count};
};

struct ContentSignal final {
    std::uint64_t id{};
    MissionText name{};
};

struct MissionTimer final {
    std::uint64_t id{};
    MissionText name{};
    std::uint32_t delayTicks{};
};

struct MissionObjective final {
    std::uint64_t id{};
    MissionText name{};
    ObjectiveState initialState{ObjectiveState::inactive};
};

struct ProximityInteraction final {
    std::uint64_t id{};
    MissionText name{};
    std::uint32_t bubble{};
    physics::world::Transform transform{};
    physics::world::Vector3 extents{};
};

struct EnemyWave final {
    std::uint64_t id{};
    MissionText name{};
    std::uint32_t spawnerDefinition{};
    std::uint8_t mode{};
    std::array<std::uint32_t, kRequestedSlotCapacity> requested{};
    std::uint8_t requestedCount{};
};

struct MissionAction final {
    MissionActionKind kind{MissionActionKind::activateWave};
    std::uint64_t target{};
    std::uint64_t value{};
};

struct MissionTransition final {
    MissionEventKind event{MissionEventKind::initialized};
    std::uint64_t source{};
    std::uint32_t requiredState{(std::numeric_limits<std::uint32_t>::max)()};
    std::array<MissionAction, kActionsPerTransition> actions{};
    std::uint8_t actionCount{};
};

struct MissionProgram final {
    MissionText identity{};
    MissionText destination{};
    std::uint32_t version{};
    MissionBudgets budgets{};
    std::array<MissionObjective, kObjectiveCapacity> objectives{};
    std::array<ProximityInteraction, kInteractionCapacity> interactions{};
    std::array<EnemyWave, kEnemyWaveCapacity> waves{};
    std::array<ContentStep, kContentStepCapacity> contentSteps{};
    std::array<ContentSignal, kContentSignalCapacity> contentSignals{};
    std::array<MissionTimer, kTimerCapacity> timers{};
    std::array<MissionTransition, kTransitionCapacity> transitions{};
    std::uint8_t objectiveCount{};
    std::uint8_t interactionCount{};
    std::uint8_t waveCount{};
    std::uint8_t contentStepCount{};
    std::uint8_t contentSignalCount{};
    std::uint8_t timerCount{};
    std::uint8_t transitionCount{};
    std::uint64_t hash{};
};

struct MissionState final {
    std::uint32_t schemaVersion{1};
    std::uint32_t missionState{};
    std::array<ObjectiveState, kObjectiveCapacity> objectives{};
    std::array<bool, kEnemyWaveCapacity> activatedWaves{};
    std::array<bool, kContentStepCapacity> activatedContentSteps{};
    std::uint64_t lastEventId{};
    std::uint64_t nextCommandId{1};
    std::array<std::uint64_t, kTransitionCapacity> processedEvents{};
    std::uint8_t processedEventCount{};
};

struct MissionEvent final {
    MissionEventKind kind{MissionEventKind::initialized};
    std::uint64_t source{};
    std::uint64_t eventId{};
    physics::world::TickId tick{};
};

struct EnemyWaveIntent final {
    std::uint64_t commandId{};
    std::uint64_t waveId{};
    std::uint32_t spawnerDefinition{};
    std::uint8_t mode{};
    std::array<std::uint32_t, kRequestedSlotCapacity> requested{};
    std::uint8_t requestedCount{};
};

struct ContentStepIntent final {
    std::uint64_t commandId{};
    std::uint64_t stepId{};
    ContentStepKind kind{ContentStepKind::count};
};

} // namespace sunrise::server::gameplay::mission
