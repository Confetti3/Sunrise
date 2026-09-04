#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "scriptable_auth_body.h"

// Constants and primitives shared by the variable-width and fixed-width scriptable-auth codecs.
// Not a public interface: only those two translation units include it.

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

inline constexpr std::uint8_t kReal32Width = 32;
inline constexpr std::uint8_t kBoolWidth = 1;
inline constexpr std::uint8_t kEnabled = 1;
inline constexpr std::uint8_t kSigned32Width = 32;
/** Signed 32-bit schema fields store zero at the middle of the unsigned wire range. */
inline constexpr std::uint32_t kSigned32Bias = 0x80000000U;
/** Two-bit mode scalars carry a bias of one, so -1 is the lowest value they can store. */
inline constexpr std::uint8_t kModeWidth = 2;
inline constexpr std::int8_t kMinimumMode = -1;
inline constexpr std::int8_t kMaximumMode = 2;
inline constexpr std::uint32_t kModeBias = 1;
/** The nested 0x80809C42 ClientRef is unset when it holds this key, type 0, and index -1. */
inline constexpr std::uint32_t kClientRefAbsentKey = 0x811C9DC5U;
inline constexpr std::uint8_t kClientRefTypeWidth = 7;
inline constexpr std::uint8_t kClientRefIndexWidth = 16;
inline constexpr std::uint32_t kClientRefIndexBias = 32'768;

/** @return True when the unused low bits in the final byte are zero. */
[[nodiscard]] inline bool finish_padding(encoding::bits::Reader& reader) noexcept {
    const std::size_t paddingBits = reader.remaining_bits();
    std::uint64_t padding = 0;
    return paddingBits < 8U && reader.read(static_cast<std::uint8_t>(paddingBits), padding)
           && padding == 0 && reader.remaining_bits() == 0;
}

/** Writes the exact nested 0x80809C42 unset ClientRef. */
[[nodiscard]] inline bool write_absent_client_ref(encoding::bits::Writer& writer) noexcept {
    return writer.write(kClientRefAbsentKey, 32) && writer.write(0, kClientRefTypeWidth)
           && writer.write(kClientRefIndexBias - 1U, kClientRefIndexWidth);
}

/** Reads and requires the exact nested 0x80809C42 unset ClientRef. */
[[nodiscard]] inline bool read_absent_client_ref(encoding::bits::Reader& reader) noexcept {
    std::uint64_t key = 0;
    std::uint64_t type = 0;
    std::uint64_t index = 0;
    return reader.read(kSigned32Width, key) && reader.read(kClientRefTypeWidth, type)
           && reader.read(kClientRefIndexWidth, index) && key == kClientRefAbsentKey && type == 0
           && index == kClientRefIndexBias - 1U;
}

/** @return True when a two-bit bias-one scalar is representable without wrapping. */
[[nodiscard]] constexpr bool valid_mode(std::int8_t mode) noexcept {
    return mode >= kMinimumMode && mode <= kMaximumMode;
}

/**
 * Advances a signed generation that the client accepts only while it stays positive.
 * @param hasLast Whether a generation was already committed.
 * @param last Last committed generation.
 * @param next Set to zero first. Receives the next generation on success.
 * @return True when the next generation is positive and did not wrap.
 */
[[nodiscard]] inline bool
next_positive_generation(bool hasLast, std::int32_t last, std::int32_t& next) noexcept {
    next = 0;
    if (hasLast && (last < 0 || last == (std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    next = hasLast ? last + 1 : 1;
    return next > 0;
}

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
