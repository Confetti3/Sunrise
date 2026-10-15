#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sunrise::middleware::crypto::sha1 {

/** SHA-1 produces 20 bytes. */
inline constexpr std::size_t kDigestSize = 20;

/** One finished SHA-1 digest. */
using Digest = std::array<std::byte, kDigestSize>;

/**
 * Hashes one buffer. Package headers store SHA-1 digests of their regions and blocks.
 * @param input Bytes to hash.
 * @param output Receives the digest only on success.
 * @return True when Windows completed the hash.
 */
[[nodiscard]] bool hash(std::span<const std::byte> input, Digest& output) noexcept;

} // namespace sunrise::middleware::crypto::sha1
