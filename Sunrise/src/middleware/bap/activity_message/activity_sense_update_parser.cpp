/**
 * The sensor sense update. Its first 129 bits are the patch epoch and one literal zero, which is
 * enough to tell whether the client still agrees with the epoch the roster update published. The
 * sense delta behind them has an unresolved width, so the rest of the body stays a bounded tail.
 */

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "sense_update.h"

namespace sunrise::middleware::bap::activity_message::sense_update {
namespace {

constexpr std::uint32_t kTrostlandGroup = 0x2986181D;
constexpr std::uint8_t kTrostlandSlotTypeWire = 2;
constexpr std::size_t kSingleObjectFixedBits = 1 + 55 + 32 + 1;
constexpr std::size_t kObjectBitsWithoutDelta = 1 + 55 + 32;
constexpr std::size_t kTwoObjectFixedBits = 2 * kObjectBitsWithoutDelta + 1;

[[nodiscard]] bool read_trostland_object(encoding::bits::Reader& reader,
                                         std::uint32_t group,
                                         std::uint16_t slotIndex,
                                         std::size_t deltaBits,
                                         std::uint32_t& generation,
                                         std::uint64_t& delta) noexcept {
    std::uint64_t value = 0;
    std::uint64_t parsedDelta = 0;
    std::uint64_t parsedGeneration = 0;
    if (!reader.read(1, value) || value != 1
        || !reader.read(32, value) || value != group
        || !reader.read(7, value) || value != kTrostlandSlotTypeWire
        || !reader.read(16, value) || value != 32768U + slotIndex
        || deltaBits == 0 || deltaBits > 64
        || !reader.read(static_cast<std::uint8_t>(deltaBits), parsedDelta)
        || !reader.read(32, parsedGeneration) || parsedGeneration == 0) {
        return false;
    }
    generation = static_cast<std::uint32_t>(parsedGeneration);
    delta = parsedDelta;
    return true;
}

} // namespace

/** Parses a sensor sense update as far as its recovered grammar reaches. */
bool parse_sense_update(std::span<const std::byte> input,
                        SenseUpdate& update,
                        std::size_t& consumedBits) noexcept {
    update = {};
    consumedBits = 0;
    if (input.size() > kOuterByteCapacity) {
        return false;
    }
    encoding::bits::Reader reader(input);
    std::uint64_t literal = 0;
    if (!reader.read(kEpochFieldWidth, update.epoch.first)
        || !reader.read(kEpochFieldWidth, update.epoch.second)
        || !reader.read(kLiteralZeroWidth, literal)) {
        update = {};
        return false;
    }
    consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
    if (literal != 0) {
        // The bit is a schema literal, so a set bit means this body is not the shape above and no
        // field read from it can be trusted.
        update = {};
        return false;
    }
    update.tailBits = static_cast<std::uint32_t>(reader.remaining_bits());
    return true;
}

bool parse_envelope(std::span<const std::byte> input,
                    Envelope& envelope,
                    std::size_t& consumedBits) noexcept {
    envelope = {};
    consumedBits = 0;
    if (input.size() > kOuterByteCapacity) {
        return false;
    }
    encoding::bits::Reader reader(input);
    std::uint64_t value = 0;
    if (!reader.read(kEpochFieldWidth, envelope.epoch.first)
        || !reader.read(kEpochFieldWidth, envelope.epoch.second)
        || !reader.read(kLiteralZeroWidth, value) || value != 0
        || !reader.read(1, value)) {
        envelope = {};
        return false;
    }
    envelope.globalDeltaPresent = value != 0;
    if (envelope.globalDeltaPresent) {
        consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
        return true;
    }

    while (true) {
        if (!reader.read(1, value)) {
            envelope = {};
            return false;
        }
        if (value == 0) {
            break;
        }
        if (envelope.groupCount >= envelope.groups.size()) {
            envelope = {};
            return false;
        }
        std::uint64_t key = 0;
        std::uint64_t bits = 0;
        if (!reader.read(32, key) || !reader.read(32, bits) || bits > kGroupBitCapacity
            || bits > reader.remaining_bits() || !reader.skip(static_cast<std::size_t>(bits))) {
            envelope = {};
            return false;
        }
        envelope.groups[envelope.groupCount++] = {
            static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(bits)};
    }
    if (!reader.read(1, value) || value != 0) {
        envelope = {};
        return false;
    }
    consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
    // Transport is byte-sized. Only zero alignment padding may follow the logical final bit.
    while (reader.remaining_bits() != 0) {
        if (!reader.read(1, value) || value != 0) {
            envelope = {};
            consumedBits = 0;
            return false;
        }
    }
    envelope.complete = true;
    return true;
}

bool parse_fixed_observation(std::span<const std::byte> input,
                             std::uint32_t group,
                             std::uint8_t wireType,
                             std::uint16_t slotIndex,
                             std::uint16_t deltaBits,
                             FixedObservation& observation) noexcept {
    observation = {};
    if (input.size() > kOuterByteCapacity || group == 0 || wireType == 0 || deltaBits == 0
        || deltaBits > kGroupBitCapacity) {
        return false;
    }
    encoding::bits::Reader reader(input);
    std::uint64_t value = 0;
    if (!reader.skip(2 * kEpochFieldWidth) || !reader.read(1, value) || value != 0
        || !reader.read(1, value) || value != 0) {
        return false;
    }
    bool foundGroup = false;
    while (true) {
        if (!reader.read(1, value)) return false;
        if (value == 0) break;
        std::uint64_t key = 0;
        std::uint64_t groupBits = 0;
        if (!reader.read(32, key) || !reader.read(32, groupBits)
            || groupBits > kGroupBitCapacity || groupBits > reader.remaining_bits()) {
            return false;
        }
        if (key != group) {
            if (!reader.skip(static_cast<std::size_t>(groupBits))) return false;
            continue;
        }
        if (foundGroup) return false;
        constexpr std::size_t kObjectFixedBits = 1 + 32 + 7 + 16 + 32;
        const std::size_t objectBits = kObjectFixedBits + deltaBits;
        if (groupBits <= 1 || (static_cast<std::size_t>(groupBits) - 1) % objectBits != 0) {
            return false;
        }
        const std::size_t objectCount = (static_cast<std::size_t>(groupBits) - 1) / objectBits;
        if (objectCount == 0) return false;
        const std::size_t before = reader.remaining_bits();
        for (std::size_t object = 0; object < objectCount; ++object) {
            std::uint64_t objectGroup = 0;
            std::uint64_t objectType = 0;
            std::uint64_t biasedSlot = 0;
            if (!reader.read(1, value) || value != 1
                || !reader.read(32, objectGroup) || objectGroup != group
                || !reader.read(7, objectType) || objectType != wireType
                || !reader.read(16, biasedSlot) || biasedSlot < 32768U) {
                return false;
            }
            std::uint64_t fingerprint = 14695981039346656037ULL;
            std::uint64_t deltaHigh = 0;
            std::uint64_t deltaLow = 0;
            const std::uint16_t highBits = deltaBits > 64 && deltaBits <= 128
                                                   ? static_cast<std::uint16_t>(deltaBits - 64)
                                                   : 0;
            for (std::uint16_t bit = 0; bit < deltaBits; ++bit) {
                if (!reader.read(1, value)) return false;
                fingerprint = (fingerprint ^ value) * 1099511628211ULL;
                if (deltaBits <= 128) {
                    std::uint64_t& word = bit < highBits ? deltaHigh : deltaLow;
                    word = (word << 1U) | value;
                }
            }
            std::uint64_t generation = 0;
            if (!reader.read(32, generation) || generation == 0) return false;
            if (biasedSlot - 32768U != slotIndex) continue;
            if (observation.matches != 0
                && (observation.generation != generation
                    || observation.deltaFingerprint != fingerprint
                    || observation.deltaHigh != deltaHigh || observation.deltaLow != deltaLow)) {
                observation = {};
                return false;
            }
            observation.generation = static_cast<std::uint32_t>(generation);
            observation.deltaBits = deltaBits;
            observation.deltaFingerprint = fingerprint;
            observation.deltaHigh = deltaHigh;
            observation.deltaLow = deltaLow;
            ++observation.matches;
        }
        if (!reader.read(1, value) || value != 0
            || before - reader.remaining_bits() != groupBits) {
            observation = {};
            return false;
        }
        foundGroup = true;
    }
    if (!reader.read(1, value) || value != 0) {
        observation = {};
        return false;
    }
    while (reader.remaining_bits() != 0) {
        if (!reader.read(1, value) || value != 0) {
            observation = {};
            return false;
        }
    }
    if (!foundGroup || observation.matches == 0) {
        observation = {};
        return false;
    }
    return true;
}

bool parse_single_bounded_observation(std::span<const std::byte> input,
                                      std::uint32_t group,
                                      std::uint8_t wireType,
                                      std::uint16_t slotIndex,
                                      std::uint16_t minimumDeltaBits,
                                      std::uint16_t maximumDeltaBits,
                                      FixedObservation& observation) noexcept {
    observation = {};
    if (group == 0 || wireType == 0 || minimumDeltaBits == 0
        || minimumDeltaBits > maximumDeltaBits || maximumDeltaBits > 89) {
        return false;
    }
    Envelope envelope{};
    std::size_t consumedBits = 0;
    if (!parse_envelope(input, envelope, consumedBits) || !envelope.complete
        || envelope.globalDeltaPresent) {
        return false;
    }
    constexpr std::uint32_t kSingleObjectGroupOverhead = 1 + 32 + 7 + 16 + 32 + 1;
    std::uint32_t matchingGroupBits = 0;
    for (std::uint32_t index = 0; index < envelope.groupCount; ++index) {
        if (envelope.groups[index].key != group) continue;
        if (matchingGroupBits != 0) return false;
        matchingGroupBits = envelope.groups[index].bits;
    }
    if (matchingGroupBits <= kSingleObjectGroupOverhead) return false;
    const std::uint32_t measuredDeltaBits = matchingGroupBits - kSingleObjectGroupOverhead;
    if (measuredDeltaBits < minimumDeltaBits || measuredDeltaBits > maximumDeltaBits) {
        return false;
    }
    if (!parse_fixed_observation(input,
                                 group,
                                 wireType,
                                 slotIndex,
                                 static_cast<std::uint16_t>(measuredDeltaBits),
                                 observation)
        || observation.matches != 1) {
        observation = {};
        return false;
    }
    return true;
}

bool parse_trostland_spawner_generation(std::span<const std::byte> input,
                                        std::uint32_t& generation,
                                        std::uint32_t& delta) noexcept {
    return parse_trostland_type1_generation(input, 271, generation, delta);
}

bool parse_trostland_type1_generation(std::span<const std::byte> input,
                                      std::uint16_t slotIndex,
                                      std::uint32_t& generation,
                                      std::uint32_t& delta) noexcept {
    return parse_type1_generation(input, kTrostlandGroup, slotIndex, generation, delta);
}

bool parse_type1_generation(std::span<const std::byte> input,
                            std::uint32_t group,
                            std::uint16_t slotIndex,
                            std::uint32_t& generation,
                            std::uint32_t& delta) noexcept {
    generation = 0;
    delta = 0;
    if (group == 0) return false;
    encoding::bits::Reader reader(input);
    std::uint64_t value = 0;
    if (!reader.skip(2 * kEpochFieldWidth) || !reader.read(1, value) || value != 0
        || !reader.read(1, value) || value != 0) {
        return false;
    }
    bool found = false;
    while (true) {
        if (!reader.read(1, value)) return false;
        if (value == 0) break;
        std::uint64_t key = 0;
        std::uint64_t groupBits = 0;
        if (!reader.read(32, key) || !reader.read(32, groupBits)
            || groupBits > reader.remaining_bits()) return false;
        if (key != group) {
            if (!reader.skip(static_cast<std::size_t>(groupBits))) return false;
            continue;
        }
        if (found || groupBits < kSingleObjectFixedBits) return false;
        const std::size_t before = reader.remaining_bits();
        const bool twoObjects = groupBits >= kTwoObjectFixedBits
                                && ((groupBits - kTwoObjectFixedBits) % 2) == 0;
        const std::size_t deltaBits = twoObjects
            ? (static_cast<std::size_t>(groupBits) - kTwoObjectFixedBits) / 2
            : static_cast<std::size_t>(groupBits) - kSingleObjectFixedBits;
        std::uint32_t firstGeneration = 0;
        std::uint64_t firstDelta = 0;
        if (!read_trostland_object(reader, group, slotIndex, deltaBits,
                                   firstGeneration, firstDelta)) return false;
        if (twoObjects) {
            std::uint32_t secondGeneration = 0;
            std::uint64_t secondDelta = 0;
            if (!read_trostland_object(reader, group, slotIndex, deltaBits,
                                       secondGeneration, secondDelta)
                || secondGeneration != firstGeneration || secondDelta != firstDelta) {
                return false;
            }
        }
        if (!reader.read(1, value) || value != 0
            || before - reader.remaining_bits() != groupBits) {
            return false;
        }
        generation = firstGeneration;
        delta = deltaBits <= 32 ? static_cast<std::uint32_t>(firstDelta) : 0;
        found = true;
    }
    if (!reader.read(1, value) || value != 0) {
        return false;
    }
    while (reader.remaining_bits() != 0) {
        if (!reader.read(1, value) || value != 0) {
            return false;
        }
    }
    return found;
}

bool parse_trostland_site0_spawn_generation(std::span<const std::byte> input,
                                            std::uint32_t& generation,
                                            std::uint32_t& delta) noexcept {
    generation = 0;
    delta = 0;
    encoding::bits::Reader reader(input);
    std::uint64_t value = 0;
    if (!reader.skip(2 * kEpochFieldWidth) || !reader.read(1, value) || value != 0
        || !reader.read(1, value) || value != 0 || !reader.read(1, value) || value != 1
        || !reader.read(32, value) || value != kTrostlandGroup
        || !reader.read(32, value) || value != 545) {
        return false;
    }

    constexpr std::size_t kDeltaBits = 48;
    constexpr std::array<std::uint16_t, 4> kSlots{220, 271, 220, 271};
    std::array<std::uint32_t, kSlots.size()> generations{};
    std::array<std::uint64_t, kSlots.size()> deltas{};
    for (std::size_t index = 0; index < kSlots.size(); ++index) {
        if (!read_trostland_object(reader,
                                   kTrostlandGroup,
                                   kSlots[index],
                                   kDeltaBits,
                                   generations[index],
                                   deltas[index])) {
            return false;
        }
    }
    if (generations[0] != generations[2] || deltas[0] != deltas[2]
        || generations[1] != generations[3] || deltas[1] != deltas[3]
        || !reader.read(1, value) || value != 0 || !reader.read(1, value) || value != 0
        || !reader.read(1, value) || value != 0) {
        return false;
    }
    while (reader.remaining_bits() != 0) {
        if (!reader.read(1, value) || value != 0) {
            return false;
        }
    }
    generation = generations[0];
    // Keep the research field bounded as before. The full 48-bit delta is only an opaque schema
    // body; generation is the authoritative transition identity.
    delta = 0;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::sense_update
