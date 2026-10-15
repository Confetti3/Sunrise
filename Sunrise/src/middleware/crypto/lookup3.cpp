#include "lookup3.h"

#include "../encoding/byte_order.h"

namespace sunrise::middleware::crypto::lookup3 {

namespace {

/** Words one main round consumes. */
constexpr std::size_t kRoundWords = 3;
/** Bytes in one word. */
constexpr std::size_t kWordBytes = encoding::kU32Size;
/** Bits in one word. */
constexpr unsigned kWordBits = 32;

/** @return Value rotated left by count. */
[[nodiscard]] constexpr std::uint32_t rotate(std::uint32_t value, unsigned count) noexcept {
    return (value << count) | (value >> (kWordBits - count));
}

/** Mixes three accumulators reversibly. Every rotation constant is part of the algorithm. */
constexpr void mix(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) noexcept {
    a -= c;
    a ^= rotate(c, 4);
    c += b;
    b -= a;
    b ^= rotate(a, 6);
    a += c;
    c -= b;
    c ^= rotate(b, 8);
    b += a;
    a -= c;
    a ^= rotate(c, 16);
    c += b;
    b -= a;
    b ^= rotate(a, 19);
    a += c;
    c -= b;
    c ^= rotate(b, 4);
    b += a;
}

/** Mixes the three accumulators one way, so only c is worth reading afterwards. */
constexpr void finish(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) noexcept {
    c ^= b;
    c -= rotate(b, 14);
    a ^= c;
    a -= rotate(c, 11);
    b ^= a;
    b -= rotate(a, 25);
    c ^= b;
    c -= rotate(b, 16);
    a ^= c;
    a -= rotate(c, 4);
    b ^= a;
    b -= rotate(a, 14);
    c ^= b;
    c -= rotate(b, 24);
}

} // namespace

/** Hashes a byte buffer as little-endian words. */
std::uint32_t hash_bytes(std::span<const std::byte> bytes, std::uint32_t initial) noexcept {
    if (bytes.size() % kWordBytes != 0) {
        return 0;
    }
    std::uint32_t a = initial;
    std::uint32_t b = initial;
    std::uint32_t c = initial;
    const std::size_t count = bytes.size() / kWordBytes;

    // Bytes are assembled, not reinterpreted: the span has no alignment guarantee.
    auto word = [bytes](std::size_t position) noexcept {
        return encoding::read_u32_le(
            bytes.subspan(position * kWordBytes).first<encoding::kU32Size>());
    };

    std::size_t index = 0;
    // The main rounds stop with one to three words left, unless the input is empty.
    while (count - index > kRoundWords) {
        a += word(index);
        b += word(index + 1);
        c += word(index + 2);
        mix(a, b, c);
        index += kRoundWords;
    }
    const std::size_t remaining = count - index;
    if (remaining == 0) {
        return c;
    }
    if (remaining == kRoundWords) {
        c += word(index + 2);
    }
    if (remaining >= 2) {
        b += word(index + 1);
    }
    a += word(index);
    finish(a, b, c);
    return c;
}

} // namespace sunrise::middleware::crypto::lookup3
