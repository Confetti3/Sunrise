#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::crypto::lookup3 {

/**
 * Hashes a byte buffer as little-endian 32-bit words with Bob Jenkins' lookup3, word form.
 * The start value is a caller literal here, not the usual length and seed.
 * @param bytes Buffer whose length must be a multiple of four.
 * @param initial Value all three accumulators start at.
 * @return The finished hash, or zero when the length is not a multiple of four.
 */
[[nodiscard]] std::uint32_t hash_bytes(std::span<const std::byte> bytes,
                                       std::uint32_t initial) noexcept;

} // namespace sunrise::middleware::crypto::lookup3
