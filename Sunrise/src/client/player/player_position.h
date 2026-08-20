#pragma once

#include <cstdint>

#include "../hooks/teleport/runtime.h"

namespace sunrise::client::player::position {

/** The local player's world position and controlled-object identity, or nothing when unseen. */
struct Snapshot {
    hooks::teleport::Vector position{};
    bool present{};
    std::uint32_t controlledHandle{};
    bool controlledHandlePresent{};
};

/** Publishes the position of the component the physics sync is running for. */
void observe(void* component) noexcept;

/** Refreshes the position for a player at rest. Call it per frame, on a game thread. */
void poll() noexcept;

/** Drops the published position. */
void reset() noexcept;

/**
 * Returns the last physics component observed for the local player.
 * The caller must revalidate ownership and resolve any body fresh before use.
 */
[[nodiscard]] void* component_candidate() noexcept;

/** @return The last published position and scalar controlled-object identity. */
[[nodiscard]] Snapshot snapshot() noexcept;

} // namespace sunrise::client::player::position
