#pragma once

#include <cstdint>

namespace sunrise::server::bap::encrypted::push::activity {


/** Manual, private-current entity-slot republish delivery counters and exact token state. */
struct EntitySlotRepublishStatus final {
    std::uint64_t requested{};
    std::uint64_t bound{};
    std::uint64_t staged{};
    std::uint64_t delivered{};
    std::uint64_t discarded{};
    std::uint64_t staleRejected{};
    std::uint64_t noPrivateRejected{};
    std::uint64_t publicRejected{};
    std::uint64_t encodeFailed{};
    std::uint64_t pendingToken{};
    std::uint64_t stagedToken{};
    std::uint64_t deliveredToken{};
    std::uint64_t pendingBindingGeneration{};
    std::uint64_t stagedBindingGeneration{};
};

/** Binds one manual one-shot diagnostic to the exact current private ActivityClient. */
[[nodiscard]] std::uint64_t request_entity_slot_republish() noexcept;
/** Copies the process-lifetime diagnostic counters without changing a request. */
[[nodiscard]] EntitySlotRepublishStatus entity_slot_republish_status() noexcept;

struct TrostlandSpawnerResearch final {
    std::uint32_t observedGeneration{};
    std::uint32_t observedDelta{};
    std::uint32_t manualGeneration{};
};

void observe_trostland_spawner_generation(std::uint32_t generation,
                                          std::uint32_t delta) noexcept;
[[nodiscard]] TrostlandSpawnerResearch trostland_spawner_research() noexcept;
void set_trostland_spawner_generation(std::uint32_t generation) noexcept;

/** Queues the single allowlisted build-86657 Glimmer intro step. */
[[nodiscard]] std::uint64_t request_glimmer_intro() noexcept;
[[nodiscard]] bool peek_glimmer_intro(std::uint64_t& token, std::uint32_t& generation) noexcept;
void commit_glimmer_intro(std::uint64_t token) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
