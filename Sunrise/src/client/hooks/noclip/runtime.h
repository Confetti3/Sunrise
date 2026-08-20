#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::noclip {

/** Three lanes of a Havok vector. A write leaves the stored fourth lane alone. */
using Vector = std::array<float, 3>;

/** Maximum number of pointer-free rigid-body observations copied from one Havok step. */
inline constexpr std::size_t kPhysicsObservationCapacity = 256;

/** One body position and velocity copied while its Havok island is owned by the step hook. */
struct PhysicsBodyObservation final {
    std::uint64_t slot{};
    Vector position{};
    Vector velocity{};
};

/** Bounded view of the body slots visited during the latest recent Havok step. */
struct PhysicsObservationSnapshot final {
    std::array<PhysicsBodyObservation, kPhysicsObservationCapacity> bodies{};
    std::uint64_t sequence{};
    std::uint32_t declaredSlots{};
    std::uint16_t bodyCount{};
    bool truncated{};
};

/**
 * Resolves the Havok targets and attaches the independent simulation-step detour.
 * @return True when both signatures resolve and the detour is attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the simulation-step detour and clears runtime state. */
[[nodiscard]] bool uninstall() noexcept;

/**
 * Copies the latest recent Havok observation without exposing any game pointer.
 * Slot values identify array locations in that snapshot, not durable body or entity identities.
 */
[[nodiscard]] bool physics_observation_snapshot(PhysicsObservationSnapshot& output) noexcept;
[[nodiscard]] bool installed() noexcept;

/**
 * Reads a live rigid body's world position.
 * @param body Body from the simulation hook. Valid only inside that call.
 * @param position Receives the three lanes.
 */
void read_body_position(void* body, Vector& position) noexcept;

/** Writes a live rigid body's world position. @see read_body_position */
void write_body_position(void* body, const Vector& position) noexcept;

/** Reads a live rigid body's linear velocity. @see read_body_position */
void read_body_velocity(void* body, Vector& velocity) noexcept;

/** Writes a live rigid body's linear velocity. @see read_body_position */
void write_body_velocity(void* body, const Vector& velocity) noexcept;

} // namespace sunrise::client::hooks::noclip
