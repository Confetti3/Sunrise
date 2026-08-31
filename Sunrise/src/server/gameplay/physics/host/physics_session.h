#pragma once

#include <cstdint>

namespace sunrise::server::gameplay::physics::host::session {

struct MissionReloadStatus final {
    std::uint64_t requested{};
    std::uint64_t completed{};
    std::uint32_t activeWorlds{};
    std::uint64_t triggerRequested{};
    std::uint64_t triggerCompleted{};
    std::uint64_t programHash{};
    std::uint32_t missionState{};
    std::uint32_t objective0{};
    std::uint32_t activatedWaveCount{};
};

/**
 * Services scriptless public-peer worlds and the exact private Trostland mission-policy world.
 * Public worlds produce no wire output. The private policy emits only generation-bound semantic
 * intents consumed by the retained private ActivityClient.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/** Queues a worker-owned close, Lua recompile, and reopen of every active mission world. */
[[nodiscard]] std::uint64_t request_mission_reload() noexcept;

/** Copies the live reload counters without crossing worker-owned pointers. */
[[nodiscard]] MissionReloadStatus mission_reload_status() noexcept;

/** Queues one named trigger-enter research input without borrowing a policy pointer. */
[[nodiscard]] std::uint64_t request_mission_trigger(std::uint64_t triggerId) noexcept;

struct MissionContentSignalReservation final { std::uint64_t sequence{}; };

/** Reserves scoped queue capacity before an inbound observation is claimed. */
[[nodiscard]] bool reserve_mission_content_signal(std::uint64_t activitySessionId,
                                                  std::uint64_t hostGeneration,
                                                  std::uint64_t bindingGeneration,
                                                  std::uint64_t signalId,
                                                  MissionContentSignalReservation& reservation) noexcept;
using MissionContentSignalCommit = bool (*)(void* context) noexcept;
/** Atomically claims the observation and publishes its reserved signal for the live scope. */
[[nodiscard]] bool commit_mission_content_signal(
    MissionContentSignalReservation reservation,
    MissionContentSignalCommit condition,
    void* context) noexcept;
/** Releases an unused or invalidated reservation. */
void abort_mission_content_signal(MissionContentSignalReservation reservation) noexcept;

/** Queues one allowlisted content observation without borrowing a policy pointer. */
[[nodiscard]] std::uint64_t request_mission_content_signal(std::uint64_t signalId) noexcept;
[[nodiscard]] std::uint64_t request_mission_content_signal(std::uint64_t activitySessionId,
                                                           std::uint64_t hostGeneration,
                                                           std::uint64_t bindingGeneration,
                                                           std::uint64_t signalId) noexcept;

/** Closes every open world and releases its State context. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::physics::host::session
