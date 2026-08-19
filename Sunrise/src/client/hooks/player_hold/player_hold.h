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

/** Observes a physics component and retains it only after local-player qualification. */
void observe_component(void* component) noexcept;

/** Captures or retrieves the session anchor and zeros velocity before Havok. */
[[nodiscard]] StepContext before_havok_step() noexcept;

/** Revalidates and restores the session anchor after all movement policies. */
void after_havok_step(const StepContext& context) noexcept;

/** Reasserts the anchor immediately before the game publishes body placement. */
void apply_sync(void* component) noexcept;

/** @return True after a validated anchor was applied in the current Viewer session. */
[[nodiscard]] bool holding() noexcept;

/** @return True while a Viewer enter/active request makes Teleport unsafe. */
[[nodiscard]] bool blocks_teleport() noexcept;

/** Clears every component candidate and copied anchor. */
void reset() noexcept;

} // namespace sunrise::client::hooks::player_hold
