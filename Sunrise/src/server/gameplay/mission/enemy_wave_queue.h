#pragma once

#include "mission_program.h"

namespace sunrise::server::gameplay::mission {

struct EnemyWaveTicket final {
    std::uint64_t value{};
};

struct QueuedEnemyWave final {
    EnemyWaveTicket ticket{};
    EnemyWaveIntent intent{};
    std::uint64_t activitySessionId{};
    /** Exact retained State record revision. */
    std::uint64_t hostGeneration{};
    /** Exact private ActivityClient lifetime that may consume this intent. */
    std::uint64_t bindingGeneration{};
    bool published{};
};

[[nodiscard]] bool reserve_enemy_wave(std::uint64_t activitySessionId,
                                      std::uint64_t hostGeneration,
                                      std::uint64_t bindingGeneration,
                                      const EnemyWaveIntent& intent,
                                      EnemyWaveTicket& ticket) noexcept;
[[nodiscard]] bool peek_enemy_wave(std::uint64_t activitySessionId,
                                   std::uint64_t hostGeneration,
                                   std::uint64_t bindingGeneration,
                                   QueuedEnemyWave& output) noexcept;
[[nodiscard]] bool mark_enemy_wave_published(EnemyWaveTicket ticket) noexcept;
[[nodiscard]] bool settle_enemy_wave(EnemyWaveTicket ticket) noexcept;
void cancel_enemy_waves(std::uint64_t activitySessionId,
                        std::uint64_t hostGeneration,
                        std::uint64_t bindingGeneration) noexcept;
void reset_enemy_waves() noexcept;

} // namespace sunrise::server::gameplay::mission
