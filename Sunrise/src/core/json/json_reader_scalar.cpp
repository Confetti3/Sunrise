#include <array>
#include <cstdint>

#include "reader.h"

namespace sunrise::core::json {
namespace {

/** JSON code points below 0x20 are unescaped control characters. */
constexpr unsigned char kFirstPrintableCodePoint = 0x20;
/** JSON allows only these one-byte escape codes after a backslash. */
constexpr std::string_view kSimpleEscapeCodes = "\"\\/bfnrt";
/** The byte each simple escape code stands for, in the same order. */
constexpr std::string_view kSimpleEscapeValues = "\"\\/\b\f\n\r\t";
/** Lowercase u starts the 4-digit JSON Unicode escape. */
constexpr char kUnicodeEscapeCode = 'u';
/** A JSON Unicode escape has exactly 4 hex digits. */
constexpr std::size_t kUnicodeEscapeDigitCount = 4;
/** UTF-16 high surrogates open a pair; the escape after one must be a low surrogate. */
constexpr std::uint32_t kHighSurrogateFirst = 0xD800;
constexpr std::uint32_t kLowSurrogateFirst = 0xDC00;
constexpr std::uint32_t kLowSurrogateLast = 0xDFFF;
/** A surrogate pair adds this base to its 20 payload bits. */
constexpr std::uint32_t kSupplementaryBase = 0x10000;
/** Each surrogate carries 10 payload bits. */
constexpr std::uint32_t kSurrogatePayloadBits = 10;
constexpr std::uint32_t kSurrogatePayloadMask = 0x3FF;
/** UTF-8 sequence limits: the last code point of each byte length. */
constexpr std::uint32_t kOneByteLast = 0x7F;
constexpr std::uint32_t kTwoByteLast = 0x7FF;
constexpr std::uint32_t kThreeByteLast = 0xFFFF;
/** UTF-8 lead and continuation markers, and the continuation payload. */
constexpr std::uint32_t kTwoByteLead = 0xC0;
constexpr std::uint32_t kThreeByteLead = 0xE0;
constexpr std::uint32_t kFourByteLead = 0xF0;
constexpr std::uint32_t kContinuationLead = 0x80;
constexpr std::uint32_t kContinuationBits = 6;
constexpr std::uint32_t kContinuationMask = 0x3F;

/**
 * Tests one byte for a hex digit, as used by JSON Unicode escapes.
 * @return True for an ASCII decimal or hex digit.
 */
[[nodiscard]] constexpr bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F')
           || (value >= 'a' && value <= 'f');
}

/** @param value Checked hex digit. @return Its value. */
[[nodiscard]] constexpr std::uint32_t hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint32_t>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint32_t>(value - 'A' + 10);
    }
    return static_cast<std::uint32_t>(value - 'a' + 10);
}

/**
 * Reads the 4-digit code unit of one `\u` escape.
 * @param raw String bytes. @param cursor Index of the first digit; advanced past the digits.
 * @param unit Receives the UTF-16 code unit.
 * @return False when fewer than 4 hex digits follow.
 */
[[nodiscard]] bool
read_code_unit(std::string_view raw, std::size_t& cursor, std::uint32_t& unit) noexcept {
    if (raw.size() - cursor < kUnicodeEscapeDigitCount) {
        return false;
    }
    unit = 0;
    for (std::size_t index = 0; index < kUnicodeEscapeDigitCount; ++index) {
        const char digit = raw[cursor + index];
        if (!is_hex_digit(digit)) {
            return false;
        }
        unit = (unit << 4U) | hex_value(digit);
    }
    cursor += kUnicodeEscapeDigitCount;
    return true;
}

/**
 * Appends one code point as UTF-8.
 * @return False when the encoded bytes do not fit.
 */
[[nodiscard]] bool
append_utf8(std::uint32_t codePoint, std::span<char> output, std::size_t& length) noexcept {
    std::array<std::uint32_t, 4> bytes{};
    std::size_t count = 0;
    if (codePoint <= kOneByteLast) {
        bytes[count++] = codePoint;
    } else if (codePoint <= kTwoByteLast) {
        bytes[count++] = kTwoByteLead | (codePoint >> kContinuationBits);
        bytes[count++] = kContinuationLead | (codePoint & kContinuationMask);
    } else if (codePoint <= kThreeByteLast) {
        bytes[count++] = kThreeByteLead | (codePoint >> (2 * kContinuationBits));
        bytes[count++] = kContinuationLead | ((codePoint >> kContinuationBits) & kContinuationMask);
        bytes[count++] = kContinuationLead | (codePoint & kContinuationMask);
    } else {
        bytes[count++] = kFourByteLead | (codePoint >> (3 * kContinuationBits));
        bytes[count++] =
            kContinuationLead | ((codePoint >> (2 * kContinuationBits)) & kContinuationMask);
        bytes[count++] = kContinuationLead | ((codePoint >> kContinuationBits) & kContinuationMask);
        bytes[count++] = kContinuationLead | (codePoint & kContinuationMask);
    }
    if (output.size() - length < count) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        output[length++] = static_cast<char>(bytes[index]);
    }
    return true;
}

/**
 * Decodes one `\u` escape, and the low half that must follow a high surrogate.
 * @param raw String bytes. @param cursor Index after the `u`; advanced past the escape.
 * @param codePoint Receives the Unicode code point.
 * @return False for bad digits or an unpaired surrogate.
 */
[[nodiscard]] bool
read_unicode_escape(std::string_view raw, std::size_t& cursor, std::uint32_t& codePoint) noexcept {
    std::uint32_t unit = 0;
    if (!read_code_unit(raw, cursor, unit)) {
        return false;
    }
    if (unit >= kLowSurrogateFirst && unit <= kLowSurrogateLast) {
        return false;
    }
    if (unit < kHighSurrogateFirst || unit >= kLowSurrogateFirst) {
        codePoint = unit;
        return true;
    }
    // A high surrogate is only valid with a `\u` low surrogate right after it.
    std::uint32_t low = 0;
    if (raw.size() - cursor < 2 || raw[cursor] != '\\' || raw[cursor + 1] != kUnicodeEscapeCode) {
        return false;
    }
    cursor += 2;
    if (!read_code_unit(raw, cursor, low) || low < kLowSurrogateFirst || low > kLowSurrogateLast) {
        return false;
    }
    codePoint = kSupplementaryBase + ((unit & kSurrogatePayloadMask) << kSurrogatePayloadBits)
                + (low & kSurrogatePayloadMask);
    return true;
}

} // namespace

/** Reads one JSON string and returns the borrowed encoded bytes. */
bool Reader::string(std::string_view& output) noexcept {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
        return false;
    }
    const std::size_t start = ++position_;
    while (position_ < input_.size()) {
        const char value = input_[position_++];
        if (value == '"') {
            output = input_.substr(start, position_ - start - 1);
            return true;
        }
        if (static_cast<unsigned char>(value) < kFirstPrintableCodePoint) {
            return false;
        }
        if (value == '\\') {
            if (position_ >= input_.size()) {
                return false;
            }
            const char escape = input_[position_++];
            if (escape != kUnicodeEscapeCode) {
                if (kSimpleEscapeCodes.find(escape) == std::string_view::npos) {
                    return false;
                }
                continue;
            }

            // A Unicode escape cannot take digits from the closing quote or the next token.
            if (input_.size() - position_ < kUnicodeEscapeDigitCount) {
                return false;
            }
            for (std::size_t index = 0; index < kUnicodeEscapeDigitCount; ++index) {
                if (!is_hex_digit(input_[position_ + index])) {
                    return false;
                }
            }
            position_ += kUnicodeEscapeDigitCount;
        }
    }
    return false;
}

/** Reads a JSON true or false literal. */
bool Reader::boolean(bool& output) noexcept {
    if (literal("true")) {
        output = true;
        return true;
    }
    if (literal("false")) {
        output = false;
        return true;
    }
    return false;
}

/** Reads one exact JSON literal after leading whitespace. */
bool Reader::literal(std::string_view value) noexcept {
    whitespace();
    if (input_.substr(position_, value.size()) != value) {
        return false;
    }
    position_ += value.size();
    return true;
}

/**
 * Decodes the escapes in one raw JSON string into UTF-8.
 * @param raw Bytes between the quotes, already checked by `Reader::string`.
 * @param output Destination storage.
 * @param length Receives the decoded byte count.
 * @return False when the text does not fit or holds an unpaired surrogate.
 */
bool decode_string(std::string_view raw, std::span<char> output, std::size_t& length) noexcept {
    length = 0;
    std::size_t cursor = 0;
    while (cursor < raw.size()) {
        const char value = raw[cursor++];
        if (value != '\\') {
            if (length == output.size()) {
                return false;
            }
            output[length++] = value;
            continue;
        }
        if (cursor >= raw.size()) {
            return false;
        }
        const char escape = raw[cursor++];
        if (escape != kUnicodeEscapeCode) {
            const std::size_t code = kSimpleEscapeCodes.find(escape);
            if (code == std::string_view::npos || length == output.size()) {
                return false;
            }
            output[length++] = kSimpleEscapeValues[code];
            continue;
        }
        std::uint32_t codePoint = 0;
        if (!read_unicode_escape(raw, cursor, codePoint)
            || !append_utf8(codePoint, output, length)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::core::json
