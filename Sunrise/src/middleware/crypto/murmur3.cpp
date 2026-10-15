#include "murmur3.h"

#include <cstdint>
#include <vector>

#include "../encoding/byte_order.h"

namespace sunrise::middleware::crypto::murmur3 {

namespace {

/** Bits in one byte. */
constexpr unsigned kByteBits = 8;
/** The variant consumes the message in 16-byte blocks. */
constexpr std::size_t kBlockSize = 16;
/** First block multiplier. */
constexpr std::uint64_t kFirstMultiplier = 0x87C37B91114253D5ULL;
/** Second block multiplier. */
constexpr std::uint64_t kSecondMultiplier = 0x4CF5AD432745937FULL;
/** First finalization multiplier. */
constexpr std::uint64_t kFirstFinal = 0xFF51AFD7ED558CCDULL;
/** Second finalization multiplier. */
constexpr std::uint64_t kSecondFinal = 0xC4CEB9FE1A85EC53ULL;
/** Additive constant of the first lane. */
constexpr std::uint64_t kFirstAddend = 0x52DCE729ULL;
/** Additive constant of the second lane. */
constexpr std::uint64_t kSecondAddend = 0x38495AB5ULL;
/** Lane mixing multiplier. */
constexpr std::uint64_t kLaneMultiplier = 5;
/** Shift the finalizer folds by. */
constexpr unsigned kFinalShift = 33;

/** @param value Input. @param count Bit count. @return The value rotated left. */
[[nodiscard]] std::uint64_t rotate(std::uint64_t value, unsigned count) noexcept {
    return (value << count) | (value >> (64U - count));
}

/** @param value Input. @return The avalanche of one lane. */
[[nodiscard]] std::uint64_t finalize(std::uint64_t value) noexcept {
    std::uint64_t mixed = value;
    mixed ^= mixed >> kFinalShift;
    mixed *= kFirstFinal;
    mixed ^= mixed >> kFinalShift;
    mixed *= kSecondFinal;
    mixed ^= mixed >> kFinalShift;
    return mixed;
}

/**
 * Reads one 64-bit word, low byte first.
 * @param input Message bytes.
 * @param offset First byte of the word.
 * @return Host-order value.
 */
[[nodiscard]] std::uint64_t read_word(std::span<const std::byte> input,
                                      std::size_t offset) noexcept {
    return encoding::read_u64_le(input.subspan(offset).first<encoding::kU64Size>());
}

} // namespace

/** Hashes two buffers as one message with the 64-bit 128-wide variant, seed zero. */
void hash(std::span<const std::byte> first,
          std::span<const std::byte> second,
          Digest& output) noexcept {
    // The variant has no streaming form, so the two parts are joined before it runs.
    std::vector<std::byte> message;
    message.reserve(first.size() + second.size());
    message.insert(message.end(), first.begin(), first.end());
    message.insert(message.end(), second.begin(), second.end());
    const std::span<const std::byte> input{message};

    std::uint64_t lowLane = 0;
    std::uint64_t highLane = 0;
    const std::size_t blocks = input.size() / kBlockSize;
    for (std::size_t block = 0; block < blocks; ++block) {
        std::uint64_t low = read_word(input, block * kBlockSize);
        std::uint64_t high = read_word(input, (block * kBlockSize) + sizeof(std::uint64_t));
        low *= kFirstMultiplier;
        low = rotate(low, 31);
        low *= kSecondMultiplier;
        lowLane ^= low;
        lowLane = rotate(lowLane, 27);
        lowLane += highLane;
        lowLane = (lowLane * kLaneMultiplier) + kFirstAddend;
        high *= kSecondMultiplier;
        high = rotate(high, 33);
        high *= kFirstMultiplier;
        highLane ^= high;
        highLane = rotate(highLane, 31);
        highLane += lowLane;
        highLane = (highLane * kLaneMultiplier) + kSecondAddend;
    }

    std::uint64_t lowTail = 0;
    std::uint64_t highTail = 0;
    const std::size_t consumed = blocks * kBlockSize;
    const std::size_t remainder = input.size() - consumed;
    for (std::size_t index = remainder; index > 0; --index) {
        const auto value = std::to_integer<std::uint64_t>(input[consumed + index - 1]);
        if (index > sizeof(std::uint64_t)) {
            highTail ^= value << ((index - 1 - sizeof(std::uint64_t)) * kByteBits);
        } else {
            lowTail ^= value << ((index - 1) * kByteBits);
        }
    }
    if (remainder > sizeof(std::uint64_t)) {
        highTail *= kSecondMultiplier;
        highTail = rotate(highTail, 33);
        highTail *= kFirstMultiplier;
        highLane ^= highTail;
    }
    if (remainder > 0) {
        lowTail *= kFirstMultiplier;
        lowTail = rotate(lowTail, 31);
        lowTail *= kSecondMultiplier;
        lowLane ^= lowTail;
    }

    const auto length = static_cast<std::uint64_t>(input.size());
    lowLane ^= length;
    highLane ^= length;
    lowLane += highLane;
    highLane += lowLane;
    lowLane = finalize(lowLane);
    highLane = finalize(highLane);
    lowLane += highLane;
    highLane += lowLane;

    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        output[index] = static_cast<std::byte>((lowLane >> (index * kByteBits)) & 0xFFU);
        output[index + sizeof(std::uint64_t)] =
            static_cast<std::byte>((highLane >> (index * kByteBits)) & 0xFFU);
    }
}

} // namespace sunrise::middleware::crypto::murmur3
