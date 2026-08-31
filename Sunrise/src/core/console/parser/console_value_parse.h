#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <system_error>
#include <string_view>

#include "../definition.h"

namespace sunrise::core::console::parser {

/**
 * Digits an integer token may carry.
 *
 * A signed 64-bit value never needs more, so a longer run is refused before it can overflow
 * rather than after.
 */
inline constexpr std::size_t kIntegerDigitLimit = 19;

/** Words accepted for a true boolean, in the forms a reader is likely to type. */
inline constexpr std::array<std::string_view, 4> kTrueWords{"true", "1", "on", "yes"};
/** Words accepted for a false boolean. */
inline constexpr std::array<std::string_view, 4> kFalseWords{"false", "0", "off", "no"};

/** @param character Byte to test. @return True for an ASCII decimal digit. */
[[nodiscard]] constexpr bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

/**
 * Reads a boolean token.
 * @param token Whole token, matched case-sensitively against the accepted words.
 * @param output Filled only on success.
 * @return `ok`, or `badArgument` when the token is not one of the accepted words.
 */
[[nodiscard]] constexpr Status parse_boolean(std::string_view token, bool& output) noexcept {
    for (const std::string_view word : kTrueWords) {
        if (token == word) {
            output = true;
            return Status::ok;
        }
    }
    for (const std::string_view word : kFalseWords) {
        if (token == word) {
            output = false;
            return Status::ok;
        }
    }
    return Status::badArgument;
}

/**
 * Reads a signed decimal token.
 *
 * Only a sign and decimal digits are accepted. A console value is typed by a reader, so no
 * radix prefix or digit separator is offered and none has to be guessed at.
 *
 * @param token Whole token.
 * @param output Filled only on success.
 * @return `ok`, or `badArgument` when the token is not a plain decimal integer.
 */
[[nodiscard]] constexpr Status parse_integer(std::string_view token,
                                             std::int64_t& output) noexcept {
    bool negative = false;
    std::size_t index = 0;
    if (index < token.size() && (token[index] == '-' || token[index] == '+')) {
        negative = token[index] == '-';
        ++index;
    }
    const std::size_t firstDigit = index;
    const std::uint64_t limit = negative
                                    ? static_cast<std::uint64_t>(
                                          (std::numeric_limits<std::int64_t>::max)())
                                          + 1U
                                    : static_cast<std::uint64_t>(
                                          (std::numeric_limits<std::int64_t>::max)());
    std::uint64_t magnitude = 0;
    for (; index < token.size(); ++index) {
        if (!is_digit(token[index]) || (index - firstDigit) >= kIntegerDigitLimit) {
            return Status::badArgument;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(token[index] - '0');
        if (magnitude > (limit - digit) / 10U) {
            return Status::badArgument;
        }
        magnitude = (magnitude * 10U) + digit;
    }
    if (index == firstDigit) {
        return Status::badArgument;
    }
    if (negative && magnitude == limit) {
        output = (std::numeric_limits<std::int64_t>::min)();
    } else {
        const auto signedMagnitude = static_cast<std::int64_t>(magnitude);
        output = negative ? -signedMagnitude : signedMagnitude;
    }
    return Status::ok;
}

/**
 * Reads a decimal token with an optional fraction.
 *
 * Optional exponent notation is accepted so the formatter can round-trip every finite value
 * without expanding tiny or large magnitudes into hundreds of fixed-point digits.
 *
 * @param token Whole token.
 * @param output Filled only on success.
 * @return `ok`, or `badArgument` when the token is not a plain decimal number.
 */
[[nodiscard]] inline Status parse_real(std::string_view token, double& output) noexcept {
    if (token.empty()) {
        return Status::badArgument;
    }
    double parsed = 0.0;
    const auto converted = std::from_chars(
        token.data(), token.data() + token.size(), parsed, std::chars_format::general);
    if (converted.ec == std::errc::result_out_of_range) {
        return Status::outOfRange;
    }
    if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size()
        || !std::isfinite(parsed)) {
        return Status::badArgument;
    }
    output = parsed;
    return Status::ok;
}

/**
 * Reads one token as a declared domain and fills a typed value.
 *
 * @param token Whole token.
 * @param type Declared domain.
 * @param output Cleared, then filled only on success.
 * @return `ok`; `badArgument` when the token does not read as the domain; `outOfRange` when text
 *         read correctly but is longer than a value may carry.
 */
[[nodiscard]] inline Status
parse_value(std::string_view token, Type type, Value& output) noexcept {
    output = Value{};
    output.type = type;
    switch (type) {
    case Type::boolean:
        return parse_boolean(token, output.boolean);
    case Type::integer:
        return parse_integer(token, output.integer);
    case Type::real:
        return parse_real(token, output.real);
    case Type::text:
        for (std::size_t index = 0; index < token.size(); ++index) {
            char character = token[index];
            if (character == '\\' && index + 1 < token.size()
                && (token[index + 1] == '\\' || token[index + 1] == '"')) {
                character = token[++index];
            }
            if (output.textLength + 1 >= output.text.size()) {
                return Status::outOfRange;
            }
            output.text[output.textLength++] = character;
        }
        return Status::ok;
    case Type::count:
        break;
    }
    return Status::badArgument;
}

/**
 * Tests a numeric value against a declared range.
 * @param value Value already read as its declared type.
 * @param minimum Inclusive low bound.
 * @param maximum Inclusive high bound. A range that is not below its low bound is unbounded.
 * @return `ok`, or `outOfRange`.
 */
[[nodiscard]] constexpr Status
check_bounds(const Value& value, double minimum, double maximum) noexcept {
    if (!(minimum < maximum)) {
        return Status::ok;
    }
    if (value.type == Type::integer) {
        constexpr double kInt64Lower = -9223372036854775808.0;
        constexpr double kInt64UpperExclusive = 9223372036854775808.0;
        const std::int64_t low = minimum <= kInt64Lower
                                     ? (std::numeric_limits<std::int64_t>::min)()
                                     : static_cast<std::int64_t>(minimum);
        const std::int64_t high = maximum >= kInt64UpperExclusive
                                      ? (std::numeric_limits<std::int64_t>::max)()
                                      : static_cast<std::int64_t>(maximum);
        return value.integer < low || value.integer > high ? Status::outOfRange : Status::ok;
    }
    if (value.type == Type::real) {
        return value.real < minimum || value.real > maximum ? Status::outOfRange : Status::ok;
    }
    return Status::ok;
}

/**
 * Tests a text value against a declared choice list.
 * @param value Value already read as text.
 * @param choices Accepted words, or empty when any text is allowed.
 * @return `ok`, or `outOfRange` when the text is not one of the choices.
 */
[[nodiscard]] constexpr Status check_choices(const Value& value,
                                             std::span<const std::string_view> choices) noexcept {
    if (choices.empty() || value.type != Type::text) {
        return Status::ok;
    }
    const std::string_view text{value.text.data(), value.textLength};
    for (const std::string_view choice : choices) {
        if (text == choice) {
            return Status::ok;
        }
    }
    return Status::outOfRange;
}

namespace detail {

/** @return The status of reading this token as this domain, for compile-time checking. */
[[nodiscard]] inline Status read_status(std::string_view token, Type type) noexcept {
    Value scratch{};
    return parse_value(token, type, scratch);
}

/** @return The real a token reads as, or the fallback when it does not read. */
[[nodiscard]] inline double read_real(std::string_view token, double fallback) noexcept {
    double value = fallback;
    return parse_real(token, value) == Status::ok ? value : fallback;
}

/** @return The integer a token reads as, or the fallback when it does not read. */
[[nodiscard]] constexpr std::int64_t read_integer(std::string_view token,
                                                  std::int64_t fallback) noexcept {
    std::int64_t value = fallback;
    return parse_integer(token, value) == Status::ok ? value : fallback;
}

} // namespace detail

// Both spellings of every boolean word are accepted, since a reader who types `on` and a settings
// file that stores `true` describe the same switch.

static_assert(detail::read_integer("42", -1) == 42);
static_assert(detail::read_integer("-42", 0) == -42);
static_assert(detail::read_integer("+7", 0) == 7);
// A run past the digit limit is refused rather than wrapped, so no token can report a magnitude
// it does not carry.
static_assert(detail::read_integer("9223372036854775807", 0)
              == (std::numeric_limits<std::int64_t>::max)());
static_assert(detail::read_integer("-9223372036854775808", 0)
              == (std::numeric_limits<std::int64_t>::min)());


// A bound is inclusive at both ends, which is what lets a reader type the documented minimum.
static_assert(check_bounds(Value{.type = Type::real, .real = 1.0}, 1.0, 100.0) == Status::ok);
static_assert(check_bounds(Value{.type = Type::real, .real = 100.0}, 1.0, 100.0) == Status::ok);
static_assert(check_bounds(Value{.type = Type::real, .real = 0.5}, 1.0, 100.0)
              == Status::outOfRange);
static_assert(check_bounds(Value{.type = Type::real, .real = 1e9}, 0.0, 0.0) == Status::ok);

} // namespace sunrise::core::console::parser
