#pragma once

#include <cstdint>

namespace sunrise::client::hooks::presentation {

/** One coherent snapshot of the independent game-presentation policies. */
struct Status {
    bool weaponReady{};
    bool hudReady{};
    bool hideWeaponRequested{};
    bool removeHudRequested{};
    bool weaponHidden{};
    bool hudHidden{};
    bool hudFault{};
};

/**
 * Resolves the native first-person rig and HUD boundaries.
 * @return True when the required
 * rig boundary is attached; HUD support is optional.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Restores owned presentation state and detaches the rig boundary.
 * @return True when owned
 * state was restored and the replacement detached.
 */
[[nodiscard]] bool uninstall() noexcept;

/**
 * Applies persisted presentation policies for the current player frame.
 * @param playerIndex Local player whose settings record is current, or the invalid handle.
 */
void apply(std::uint32_t playerIndex) noexcept;

/** @return One coherent readiness and policy snapshot. */
[[nodiscard]] Status status() noexcept;

} // namespace sunrise::client::hooks::presentation
