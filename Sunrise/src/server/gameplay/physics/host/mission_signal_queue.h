#pragma once

#include <cstdint>

namespace sunrise::server::gameplay::physics::host::mission_signal_queue {

struct Request final {
    std::uint64_t id{};
    std::uint64_t sequence{};
    std::uint64_t activitySessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t bindingGeneration{};
    bool scoped{};
    bool ready{};
    bool occupied{};
};

[[nodiscard]] bool register_scope(std::uint64_t activitySessionId,
                                  std::uint64_t hostGeneration,
                                  std::uint64_t bindingGeneration) noexcept;
void unregister_scope(std::uint64_t activitySessionId,
                      std::uint64_t hostGeneration,
                      std::uint64_t bindingGeneration) noexcept;
[[nodiscard]] std::uint64_t reserve(std::uint64_t activitySessionId,
                                    std::uint64_t hostGeneration,
                                    std::uint64_t bindingGeneration,
                                    std::uint64_t signalId,
                                    bool scoped,
                                    bool ready) noexcept;
using CommitCondition = bool (*)(void* context) noexcept;
/** Verifies the live reservation and runs its observation claim while holding the queue lock. */
[[nodiscard]] bool commit_if(std::uint64_t sequence,
                             CommitCondition condition,
                             void* context) noexcept;
[[nodiscard]] bool commit(std::uint64_t sequence) noexcept;
[[nodiscard]] bool peek(std::uint64_t activitySessionId,
                        std::uint64_t hostGeneration,
                        std::uint64_t bindingGeneration,
                        Request& request) noexcept;
[[nodiscard]] bool consume(std::uint64_t sequence) noexcept;
void reset() noexcept;

} // namespace sunrise::server::gameplay::physics::host::mission_signal_queue
