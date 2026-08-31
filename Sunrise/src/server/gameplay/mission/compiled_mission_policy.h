#pragma once

#include "mission_program.h"
#include "../physics/world/activity_policy.h"

namespace sunrise::server::gameplay::mission {

class CompiledMissionPolicy final : public physics::world::IActivityPolicy {
public:
    [[nodiscard]] bool configure(const MissionProgram& program,
                                 std::uint64_t contentBuildId) noexcept;
    [[nodiscard]] physics::world::ActivityPolicyManifest manifest() const noexcept override;
    [[nodiscard]] bool initialize(const physics::world::ActivityPolicyContext& context,
                                  physics::world::HostCommands& commands) noexcept override;
    void pre_tick(const physics::world::PolicyTickContext& context,
                  physics::world::HostCommands& commands) noexcept override;
    void post_tick(const physics::world::PolicyTickContext& context,
                   std::span<const physics::world::CommittedEvent> committedEvents,
                   physics::world::HostCommands& commands) noexcept override;
    [[nodiscard]] bool save(physics::world::IPolicyStateWriter& writer) const noexcept override;
    [[nodiscard]] bool load(physics::world::IPolicyStateReader& reader) noexcept override;

    [[nodiscard]] bool pop_enemy_wave_intent(EnemyWaveIntent& intent) noexcept;
    /** Reads the next content intent without consuming it, so downstream backpressure is retryable. */
    [[nodiscard]] bool peek_content_step_intent(ContentStepIntent& intent) const noexcept;
    /** Consumes the exact head previously peeked; mismatched or stale callers fail closed. */
    [[nodiscard]] bool consume_content_step_intent(const ContentStepIntent& expected) noexcept;
    [[nodiscard]] bool pop_content_step_intent(ContentStepIntent& intent) noexcept;
    /** Distinguishes a permanently unknown signal from a temporarily busy policy. */
    [[nodiscard]] bool knows_content_signal(std::uint64_t signalId) const noexcept;
    /** Queues one ordered research input for the next policy tick. */
    [[nodiscard]] bool queue_trigger_enter(std::uint64_t triggerId,
                                           std::uint64_t requestId) noexcept;
    [[nodiscard]] bool queue_content_signal(std::uint64_t signalId,
                                            std::uint64_t observationId) noexcept;
    [[nodiscard]] const MissionState& state() const noexcept { return state_; }
    [[nodiscard]] const MissionProgram& program() const noexcept { return program_; }

private:
    struct Persisted final {
        std::uint64_t programHash{};
        MissionState state{};
        std::array<EnemyWaveIntent, kEnemyWaveIntentCapacity> intents{};
        std::uint8_t intentHead{};
        std::uint8_t intentCount{};
        std::array<ContentStepIntent, kContentStepIntentCapacity> contentIntents{};
        std::uint8_t contentIntentHead{};
        std::uint8_t contentIntentCount{};
    };

    [[nodiscard]] physics::world::PolicyCommandMeta command_meta(
        physics::world::TickId tick) noexcept;
    [[nodiscard]] bool process_event(const MissionEvent& event,
                                     physics::world::HostCommands& commands) noexcept;
    [[nodiscard]] bool process_action(const MissionAction& action,
                                      physics::world::TickId tick,
                                      physics::world::HostCommands& commands) noexcept;

    MissionProgram program_{};
    physics::world::ActivityPolicyManifest manifest_{};
    MissionState state_{};
    std::array<EnemyWaveIntent, kEnemyWaveIntentCapacity> intents_{};
    std::uint8_t intentHead_{};
    std::uint8_t intentCount_{};
    std::array<ContentStepIntent, kContentStepIntentCapacity> contentIntents_{};
    std::uint8_t contentIntentHead_{};
    std::uint8_t contentIntentCount_{};
    bool configured_{};
    bool initialized_{};
    bool restartCooldownTimerPending_{};
    MissionEvent pendingResearchEvent_{};
    bool researchEventPending_{};
};

} // namespace sunrise::server::gameplay::mission
