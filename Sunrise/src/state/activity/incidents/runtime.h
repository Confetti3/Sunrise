#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity::incidents {

/** Msg-19 validation bounds this copied list before publication. */
inline constexpr std::size_t kExtraTargetCapacity = 25;
/** Largest selector-free msg-19 body accepted by the validator, rounded up to whole bytes. */
inline constexpr std::size_t kBodyCapacity = 553;
/** A slow or absent script host can retain at most this many copied observations. */
inline constexpr std::size_t kObservationCapacity = 64;

/**
 * Pointer-free copy of one validated client-to-host activity incident.
 * The queue assigns sequence, observation tick, and droppedBefore during publication.
 */
struct Observation final {
    std::array<std::uint32_t, kExtraTargetCapacity> extraTargets{};
    /** Exact validated body retained in-process for a later controlled replay probe. */
    std::array<std::byte, kBodyCapacity> body{};
    std::uint64_t sequence{};
    std::uint64_t observedAtTickMs{};
    std::uint64_t sessionId{};
    std::uint64_t accountHandle{};
    std::uint64_t droppedBefore{};
    /** FNV-1a fingerprint of body[0..bodyLength), exposed instead of the body bytes. */
    std::uint64_t bodyFingerprint{};
    std::uint32_t primaryTarget{};
    std::uint32_t extraTargetCount{};
    std::uint32_t payloadLength{};
    std::uint32_t bodyLength{};
    bool hasCompressedSelector{};
    bool hasPayload{};
};

/**
 * Publishes one copied observation without blocking the activity-message route.
 * @return False when the queue lock was busy or the copied shape was invalid.
 */
[[nodiscard]] bool publish(Observation observation) noexcept;

/** Removes the oldest queued observation. Only the script-host worker calls this. */
[[nodiscard]] bool try_pop(Observation& observation) noexcept;

/**
 * Discards observations captured before a new bridge connection.
 * @return Number discarded, also included in the monotonic dropped count.
 */
[[nodiscard]] std::size_t discard_pending() noexcept;

/** @return Total observations dropped because of contention, overflow, or reconnect. */
[[nodiscard]] std::uint64_t dropped_count() noexcept;

} // namespace sunrise::state::activity::incidents
