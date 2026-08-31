#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::incident {

/** Activity message type 19 carries one incident. Both sides can send it. */
inline constexpr std::uint32_t kMessageType = 19;

inline constexpr std::uint8_t kTargetWidth = 13;
/** The highest valid target index. Above it the Client indexes handler tables unbounded. */
inline constexpr std::uint32_t kTargetMaximum = 7'762;
/** These three rows carry type code -1 and are a crash risk, so they never pass. */
inline constexpr std::array<std::uint32_t, 3> kPoisonTargets{795, 4'690, 5'375};

inline constexpr std::uint8_t kExtraCountWidth = 5;
inline constexpr std::uint32_t kExtraTargetMaximum = 25;
inline constexpr std::uint8_t kSelectorPresenceWidth = 1;
inline constexpr std::uint8_t kSelectorLengthWidth = 9;
inline constexpr std::uint32_t kSelectorMaximum = 260;
inline constexpr std::uint8_t kOptionalPresenceWidth = 1;
inline constexpr std::uint8_t kOptionalWordWidth = 32;
inline constexpr std::uint8_t kPayloadLengthWidth = 9;
inline constexpr std::uint32_t kPayloadMaximum = 500;
/** Why one incident did not pass validation. */
enum class Verdict : std::uint8_t {
    accepted,
    truncated,
    targetOutOfRange,
    targetPoisoned,
    tooManyTargets,
    payloadTooLong,
    selectorTooLong,
};

/** One validated incident, framed to the end of its payload. */
struct Incident {
    std::uint32_t primaryTarget{};
    std::array<std::uint16_t, kExtraTargetMaximum> extraTargets{};
    std::uint32_t extraTargetCount{};
    std::uint32_t selectorLength{};
    std::uint32_t payloadLength{};
    /** Bits the body used. Below the payload bit count means trailing padding. */
    std::uint32_t consumedBits{};
    /** Set when the two optional words are present. */
    bool hasOptionalBlock{};
};

/** @return A short stable name for one verdict, for the log line. */
[[nodiscard]] const char* verdict_name(Verdict verdict) noexcept;

/** Validates framing and every target before the Client can consume the incident. */
[[nodiscard]] Verdict validate(std::span<const std::byte> payload, Incident& parsed) noexcept;

} // namespace sunrise::middleware::bap::activity_message::incident
