#pragma once

#include <array>
#include <cstdint>

namespace sunrise::client::hooks::player_hold {

using Vector = std::array<float, 3>;

/** One transient proof carried only across the original Havok call. */
struct StepContext {
    Vector anchor{};
    void* body{};
    std::uintptr_t componentIdentity{};
    std::uintptr_t bodyIdentity{};
    std::uint64_t session{};
    bool valid{};
};

enum class ReanchorResult : std::uint8_t {
    inactive,
    updated,
    failed,
};

/**
 * Captures or retrieves the session anchor and zeros velocity before Havok.
 * @return
 * Transient ownership proof for the matching synchronous post-step call.
 */
[[nodiscard]] StepContext before_havok_step() noexcept;

/**
 * Revalidates and restores the session anchor after all movement policies.
 * @param context
 * Transient ownership proof captured before the same Havok call.
 */
void after_havok_step(const StepContext& context) noexcept;

/**
 * Reasserts the anchor immediately before the game publishes body placement.
 * @param
 * component Transient component candidate from the synchronous physics callback.
 */
void apply_sync(void* component) noexcept;

/** @return True after a validated anchor was applied in the current Viewer session. */
[[nodiscard]] bool holding() noexcept;

/** @return True while a Viewer enter/active request makes Teleport unsafe. */
[[nodiscard]] bool blocks_teleport() noexcept;

/**
 * Replaces the current Viewer-session anchor and applies it to the validated
 * local-player body.
 * Called only from the teleport physics callback.
 * @param component Transient component candidate
 * from the teleport callback.
 * @param position Finite world-space anchor requested by the
 * caller.
 * @return Outcome distinguishing an inactive session, an update, and validation
 * failure.
 */
[[nodiscard]] ReanchorResult reanchor(void* component, const Vector& position) noexcept;

/** Clears the copied session anchor. */
void reset() noexcept;

} // namespace sunrise::client::hooks::player_hold
