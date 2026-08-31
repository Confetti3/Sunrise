#pragma once

#include "mission_program.h"

namespace sunrise::server::gameplay::mission {

struct ContentStepTicket final { std::uint64_t value{}; };

struct ContentStepObservation final {
    ContentStepTicket ticket{};
    std::uint64_t activitySessionId{};
    std::uint64_t hostGeneration{};
    std::uint64_t bindingGeneration{};
    std::uint64_t stepId{};
    std::uint32_t generation{};
};

enum class ContentStepStage : std::uint8_t { publish };

struct QueuedContentStep final {
    ContentStepTicket ticket{};
    ContentStepIntent intent{};
    std::uint64_t activitySessionId{};
    /** Exact retained State record revision. */
    std::uint64_t hostGeneration{};
    /** Exact private ActivityClient lifetime that may consume this intent. */
    std::uint64_t bindingGeneration{};
    ContentStepStage stage{ContentStepStage::publish};
    /** Producer consumed the exact policy FIFO head; only then may BAP stage this ticket. */
    bool producerReady{};
    /** BAP owns this exact slot until its copied body is committed or discarded. */
    bool staged{};
    /** Lifecycle cancellation waits for an in-flight staged body to resolve. */
    bool cancelPending{};
    bool published{};
    /** Outbound delivery settled; observation-bearing spawn tickets remain claimable. */
    bool settled{};
    /** One authoritative client observation may claim this command. */
    bool observed{};
    /** Exact private-lifetime generation observed before outbound publication. */
    std::uint32_t baselineGeneration{};
    bool baselineReady{};
    /** Exact monotonically advancing object generation that claimed this ticket. */
    std::uint32_t observationGeneration{};
};

[[nodiscard]] bool reserve_content_step(std::uint64_t activitySessionId,
                                        std::uint64_t hostGeneration,
                                        std::uint64_t bindingGeneration,
                                        const ContentStepIntent& intent,
                                        ContentStepTicket& ticket) noexcept;
[[nodiscard]] bool peek_content_step(std::uint64_t activitySessionId,
                                     std::uint64_t hostGeneration,
                                     std::uint64_t bindingGeneration,
                                     QueuedContentStep& output) noexcept;
/** Pins the exact FIFO head while BAP owns a copied-but-uncommitted roster body. */
[[nodiscard]] bool stage_content_step(std::uint64_t activitySessionId,
                                      std::uint64_t hostGeneration,
                                      std::uint64_t bindingGeneration,
                                      QueuedContentStep& output) noexcept;
/** Makes one provisional reservation visible after the exact policy head is consumed. */
[[nodiscard]] bool commit_content_step(ContentStepTicket ticket) noexcept;
/** Commits a pinned BAP body and returns its immutable queue identity. */
[[nodiscard]] bool commit_staged_content_step(ContentStepTicket ticket,
                                              QueuedContentStep& output) noexcept;
/** Releases a pinned BAP body that never reached the caller. */
[[nodiscard]] bool discard_staged_content_step(ContentStepTicket ticket) noexcept;
/** Records an exact-lifetime pre-publication baseline, then claims only a newer observation. */
[[nodiscard]] bool claim_content_step_observation(std::uint64_t activitySessionId,
                                                  std::uint64_t hostGeneration,
                                                  std::uint64_t bindingGeneration,
                                                  std::uint64_t stepId,
                                                  std::uint32_t generation,
                                                  ContentStepObservation& observation) noexcept;
[[nodiscard]] bool settle_content_step(ContentStepTicket ticket) noexcept;
/** Cancels one provisional reservation that was never made visible to BAP. */
[[nodiscard]] bool cancel_content_step(ContentStepTicket ticket) noexcept;
void cancel_content_steps(std::uint64_t activitySessionId,
                          std::uint64_t hostGeneration,
                          std::uint64_t bindingGeneration) noexcept;
void reset_content_steps() noexcept;

} // namespace sunrise::server::gameplay::mission
