#pragma once

#include <array>
#include <cstdint>

#include "../../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../../middleware/gameplay/external/common_state.h"

namespace sunrise::state::gameplay::external::common_reconciler {

/** One peer's progress through the client-selected common-generation barrier. */
enum class Phase : std::uint8_t {
    closed,
    awaitingInitial,
    requestPending,
    awaitingConfirmation,
    ready,
    failed,
};

/** Result of one inbound common-root observation. */
enum class ObserveResult : std::uint8_t {
    initialAccepted,
    awaitingRequestedGeneration,
    ready,
    notOpen,
    wrongShape,
    wrongEpoch,
    wrongSession,
    unexpectedGeneration,
};

/**
 * Reconciles the generation the client first publishes with activity message 44.
 * The initial generation is never guessed. A queued message 44 advances only the request half;
 * entity output remains closed until a later client common root confirms the requested byte.
 */
class Reconciler final {
public:
    /** Clears every retained identity and generation. */
    void reset() noexcept;

    /** Opens one exact activity/peer epoch. Patch-epoch words may legitimately be zero. */
    [[nodiscard]] bool
    open(std::uint64_t activitySessionId,
         const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
         std::uint64_t ownerGeneration) noexcept;

    /** Opens from the replication epoch this host already authored in the join result. */
    [[nodiscard]] bool
    open_known(std::uint64_t activitySessionId,
               const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
               std::uint64_t ownerGeneration,
               std::uint8_t replicationEpoch) noexcept;

    /** Observes one complete client common root and advances only on the expected generation. */
    [[nodiscard]] ObserveResult
    observe(const middleware::gameplay::external::CommonState& common) noexcept;

    /** @return True while one never-committed message-44 generation is owed. */
    [[nodiscard]] bool pending_request(std::uint8_t& generation) const noexcept;

    /** Commits the message-44 request after its reliable enqueue succeeds. */
    [[nodiscard]] bool commit_request() noexcept;

    /** Copies the exact one-entry common root only after the client confirms it. */
    [[nodiscard]] bool
    outbound_common(middleware::gameplay::external::CommonState& common) const noexcept;

    /** @return Current barrier phase. */
    [[nodiscard]] Phase phase() const noexcept;
    /** @return Process-local owner generation for stale-work rejection. */
    [[nodiscard]] std::uint64_t owner_generation() const noexcept;
    /** @return First client-selected generation; phase distinguishes a legitimate zero. */
    [[nodiscard]] std::uint8_t initial_generation() const noexcept;
    /** @return Requested generation; phase distinguishes a legitimate zero. */
    [[nodiscard]] std::uint8_t requested_generation() const noexcept;

private:
    /** Validates count, epoch, and session before generation is considered. */
    [[nodiscard]] ObserveResult
    validate(const middleware::gameplay::external::CommonState& common) noexcept;
    /** Moves the barrier to failed and returns the supplied reason. */
    [[nodiscard]] ObserveResult fail(ObserveResult reason) noexcept;

    std::array<std::uint64_t, 2> patchEpoch_{};
    std::uint64_t activitySessionId_{};
    std::uint64_t ownerGeneration_{};
    std::uint8_t initialGeneration_{};
    std::uint8_t requestedGeneration_{};
    Phase phase_{Phase::closed};
};

} // namespace sunrise::state::gameplay::external::common_reconciler
