#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "activity_patch_epoch_parser.h"

namespace sunrise::middleware::bap::activity_message::sense_update {

/** The client reports sensor sense changes. It is the client's answer to the roster update. */
inline constexpr std::uint32_t kMessageType = 6;

/** The body opens with the same 128-bit patch epoch the roster update echoes. */
inline constexpr std::uint8_t kEpochFieldWidth = 64;
/** One literal zero bit follows the epoch. A set bit means the body is not this shape. */
inline constexpr std::uint8_t kLiteralZeroWidth = 1;
/** The client's outer destination bounds the whole body. */
inline constexpr std::size_t kOuterByteCapacity = 514'048;
/** The client's per-group scratch bounds one group substream. */
inline constexpr std::size_t kGroupBitCapacity = 102'400;
/** Debug metadata retained from one body; excess groups are rejected, not truncated. */
inline constexpr std::size_t kGroupMetadataCapacity = 64;

/**
 * The recovered prefix of a sensor sense update.
 * The sense delta behind the literal zero has an unresolved width, so the group loop after it
 * cannot be located and everything from the delta on is one bounded tail.
 */
struct SenseUpdate {
    /** Epoch the client believes is current. It must match the one the roster update carried. */
    patch_epoch::PatchEpoch epoch{};
    /** Bits left after the literal zero. Their grammar is unresolved. */
    std::uint32_t tailBits{};
};

/** One length-delimited entity-group envelope, independent of its object schemas. */
struct GroupEnvelope final {
    std::uint32_t key{};
    std::uint32_t bits{};
};

/** Schema-independent framing recovered for a complete message-6 body. */
struct Envelope final {
    patch_epoch::PatchEpoch epoch{};
    std::array<GroupEnvelope, kGroupMetadataCapacity> groups{};
    std::uint32_t groupCount{};
    /** A present global delta needs schema 0x80809445 before groups can be located. */
    bool globalDeltaPresent{};
    /** True when the group terminator and final literal zero were both located. */
    bool complete{};
};

/** One fail-closed observation from an exact group/type/slot and measured schema width. */
struct FixedObservation final {
    std::uint32_t generation{};
    std::uint16_t deltaBits{};
    std::uint16_t matches{};
    /** FNV-1a over the delta bits in wire order; correlation only, not a decoded value. */
    std::uint64_t deltaFingerprint{};
    /** Up to 128 opaque wire bits, split before the last 64 bits and right-aligned. */
    std::uint64_t deltaHigh{};
    std::uint64_t deltaLow{};
};

/**
 * Parses a sensor sense update as far as its recovered grammar reaches.
 * @param input Activity payload after the envelope.
 * @param update Cleared first, then filled with the epoch and the tail size.
 * @param consumedBits Receives the bits the recovered prefix used.
 * @return True when the epoch and the literal zero were both present and the zero read zero.
 */
[[nodiscard]] bool parse_sense_update(std::span<const std::byte> input,
                                      SenseUpdate& update,
                                      std::size_t& consumedBits) noexcept;

/**
 * Walks the documented length-delimited group envelope when the global delta is absent.
 * Object bodies remain opaque and are skipped by their exact authored group bit length.
 */
[[nodiscard]] bool parse_envelope(std::span<const std::byte> input,
                                  Envelope& envelope,
                                  std::size_t& consumedBits) noexcept;

/**
 * Parses repeated fixed-width object observations without assigning gameplay meaning to the delta.
 * Every object in the matching group must use the supplied wire type and delta width. A target
 * repeated in the same body must have the same generation and fingerprint or the body is refused.
 */
[[nodiscard]] bool parse_fixed_observation(std::span<const std::byte> input,
                                           std::uint32_t group,
                                           std::uint8_t wireType,
                                           std::uint16_t slotIndex,
                                           std::uint16_t deltaBits,
                                           FixedObservation& observation) noexcept;

/**
 * Parses exactly one object from the matching group and derives its bounded opaque delta width
 * from the authored group envelope. The maximum is capped at 89 bits: even two one-bit objects
 * need a larger group, so mixed and repeated groups cannot alias this shape. This is variable-width
 * research telemetry only and does not assign gameplay meaning to any bit.
 */
[[nodiscard]] bool parse_single_bounded_observation(std::span<const std::byte> input,
                                                    std::uint32_t group,
                                                    std::uint8_t wireType,
                                                    std::uint16_t slotIndex,
                                                    std::uint16_t minimumDeltaBits,
                                                    std::uint16_t maximumDeltaBits,
                                                    FixedObservation& observation) noexcept;

/**
 * Reads the generation from the confirmed Trostland spawner report shapes. The client has now
 * emitted both one object and two identical object blocks; any other identity, count, or mixed
 * generation is refused. Deltas wider than the returned 32-bit research field are skipped and
 * reported as zero.
 */
[[nodiscard]] bool parse_trostland_spawner_generation(std::span<const std::byte> input,
                                                      std::uint32_t& generation,
                                                      std::uint32_t& delta) noexcept;

/** Strictly parses one duplicated build-86657 type-1 object report for the requested slot. */
[[nodiscard]] bool parse_trostland_type1_generation(std::span<const std::byte> input,
                                                    std::uint16_t slotIndex,
                                                    std::uint32_t& generation,
                                                    std::uint32_t& delta) noexcept;

/** Strict one/two-object type-1 report parser for an exact build-profile tuple. */
[[nodiscard]] bool parse_type1_generation(std::span<const std::byte> input,
                                          std::uint32_t group,
                                          std::uint16_t slotIndex,
                                          std::uint32_t& generation,
                                          std::uint32_t& delta) noexcept;

/**
 * Parses the first site activation report. Build 86657 emits the new dropship slot 220 and the
 * already-live defender slot 271 twice each, in that order, under their shared type-1 schema.
 */
[[nodiscard]] bool parse_trostland_site0_spawn_generation(std::span<const std::byte> input,
                                                          std::uint32_t& generation,
                                                          std::uint32_t& delta) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sense_update
