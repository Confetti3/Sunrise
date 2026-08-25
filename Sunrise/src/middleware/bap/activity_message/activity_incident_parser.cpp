/**
 * Incident targets index a 7,763-record table that the Client reads without a bound check, so a
 * bad index is a crash and not a decode error. Rows 795, 4690 and 5375 hold type code -1 and are
 * the same risk. This validator rejects both before anything acts on the body.
 * The compressed target selector carries its own 9-bit byte length, so the fields behind it are
 * located and the whole body is framed.
 */

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "incident.h"

namespace sunrise::middleware::bap::activity_message::incident {
namespace {

/** Only this header shape has a validated payload layout for the decode below. */
inline constexpr std::uint32_t kPositionHeaderBits = 29;
/** Bit offset of the position triple, MSB-first from the start of the raw message body. */
inline constexpr std::size_t kPositionBitOffset = 518;
/** Three consecutive 32-bit fields: x, y, z. */
inline constexpr std::size_t kPositionBitWidth = 96;
/** A decoded axis magnitude at or above this is treated as garbage rather than a real position. */
inline constexpr float kPositionSanityLimit = 100'000.0F;

/** @return True when one target index is safe to hand to the Client's table lookup. */
[[nodiscard]] bool target_allowed(std::uint32_t target, Verdict& verdict) noexcept {
    if (target > kTargetMaximum) {
        verdict = Verdict::targetOutOfRange;
        return false;
    }
    if (std::find(kPoisonTargets.begin(), kPoisonTargets.end(), target) != kPoisonTargets.end()) {
        verdict = Verdict::targetPoisoned;
        return false;
    }
    return true;
}

/** @return True when a decoded axis is finite and inside the sanity bound. */
[[nodiscard]] bool position_axis_sane(float value) noexcept {
    return std::isfinite(value) && std::fabs(value) < kPositionSanityLimit;
}

/**
 * Decodes the validated position triple directly from the raw message body, independent of the
 * header reader's cursor. Only called once the headerBits == 29 shape and buffer length are
 * confirmed by the caller.
 * @param body The exact span validate() was given.
 * @param parsed Receives x/y/z and hasPosition when the read and sanity gate both succeed.
 */
void decode_position(std::span<const std::byte> body, Incident& parsed) noexcept {
    encoding::bits::Reader positionReader(body);
    std::uint64_t xWord = 0;
    std::uint64_t yWord = 0;
    std::uint64_t zWord = 0;
    if (!positionReader.skip(kPositionBitOffset) || !positionReader.read(32, xWord)
        || !positionReader.read(32, yWord) || !positionReader.read(32, zWord)) {
        return;
    }
    const float x = std::bit_cast<float>(static_cast<std::uint32_t>(xWord));
    const float y = std::bit_cast<float>(static_cast<std::uint32_t>(yWord));
    const float z = std::bit_cast<float>(static_cast<std::uint32_t>(zWord));
    if (!position_axis_sane(x) || !position_axis_sane(y) || !position_axis_sane(z)) {
        return;
    }
    parsed.x = x;
    parsed.y = y;
    parsed.z = z;
    parsed.hasPosition = true;
}

} // namespace

/** @return A short stable name for one verdict, for the log line. */
const char* verdict_name(Verdict verdict) noexcept {
    switch (verdict) {
    case Verdict::accepted:
        return "accepted";
    case Verdict::truncated:
        return "truncated";
    case Verdict::targetOutOfRange:
        return "target_out_of_range";
    case Verdict::targetPoisoned:
        return "target_poisoned";
    case Verdict::tooManyTargets:
        return "too_many_targets";
    case Verdict::payloadTooLong:
        return "payload_too_long";
    case Verdict::selectorTooLong:
        return "selector_too_long";
    }
    return "unknown";
}

/** Validates one incident body from its first target to the end of its payload. */
Verdict validate(std::span<const std::byte> payload, Incident& parsed) noexcept {
    parsed = {};
    encoding::bits::Reader reader(payload);

    std::uint64_t field = 0;
    if (!reader.read(kTargetWidth, field)) {
        return Verdict::truncated;
    }
    parsed.primaryTarget = static_cast<std::uint32_t>(field);
    Verdict verdict = Verdict::accepted;
    if (!target_allowed(parsed.primaryTarget, verdict)) {
        return verdict;
    }

    if (!reader.read(kExtraCountWidth, field)) {
        return Verdict::truncated;
    }
    parsed.extraTargetCount = static_cast<std::uint32_t>(field);
    if (parsed.extraTargetCount > kExtraTargetMaximum) {
        return Verdict::tooManyTargets;
    }
    for (std::uint32_t index = 0; index < parsed.extraTargetCount; ++index) {
        if (!reader.read(kTargetWidth, field)) {
            return Verdict::truncated;
        }
        parsed.extraTargets[index] = static_cast<std::uint32_t>(field);
        if (!target_allowed(parsed.extraTargets[index], verdict)) {
            return verdict;
        }
    }

    if (!reader.read(kSelectorPresenceWidth, field)) {
        return Verdict::truncated;
    }
    parsed.hasCompressedSelector = field != 0;
    if (parsed.hasCompressedSelector) {
        if (!reader.read(kSelectorLengthWidth, field)) {
            return Verdict::truncated;
        }
        parsed.selectorLength = static_cast<std::uint32_t>(field);
        if (parsed.selectorLength > kSelectorMaximum) {
            return Verdict::selectorTooLong;
        }
        for (std::uint32_t index = 0; index < parsed.selectorLength; ++index) {
            if (!reader.read(CHAR_BIT, field)) {
                return Verdict::truncated;
            }
            parsed.selector[index] = static_cast<std::byte>(field);
        }
    }

    if (!reader.read(kOptionalPresenceWidth, field)) {
        return Verdict::truncated;
    }
    parsed.hasOptionalBlock = field != 0;
    if (parsed.hasOptionalBlock) {
        std::uint64_t wordB = 0;
        if (!reader.read(kOptionalWordWidth, field) || !reader.read(kOptionalWordWidth, wordB)) {
            return Verdict::truncated;
        }
        parsed.optionalWordA = static_cast<std::uint32_t>(field);
        parsed.optionalWordB = static_cast<std::uint32_t>(wordB);
    }

    if (!reader.read(kPayloadLengthWidth, field)) {
        return Verdict::truncated;
    }
    parsed.payloadLength = static_cast<std::uint32_t>(field);
    if (parsed.payloadLength > kPayloadMaximum) {
        return Verdict::payloadTooLong;
    }
    // The header is everything decoded up to here; the payload bytes read below are opaque.
    parsed.headerBits = static_cast<std::uint32_t>(payload.size() * encoding::kBitsPerByte
                                                    - reader.remaining_bits());
    // The position triple is only validated for this one header shape, and only when the body is
    // long enough to actually hold bits 518..613.
    if (parsed.headerBits == kPositionHeaderBits
        && payload.size() * encoding::kBitsPerByte >= kPositionBitOffset + kPositionBitWidth) {
        decode_position(payload, parsed);
    }
    for (std::uint32_t index = 0; index < parsed.payloadLength; ++index) {
        if (!reader.read(CHAR_BIT, field)) {
            return Verdict::truncated;
        }
        parsed.payload[index] = static_cast<std::byte>(field);
    }
    parsed.hasPayload = true;
    parsed.consumedBits = static_cast<std::uint32_t>(payload.size() * encoding::kBitsPerByte
                                                     - reader.remaining_bits());
    return Verdict::accepted;
}

} // namespace sunrise::middleware::bap::activity_message::incident
