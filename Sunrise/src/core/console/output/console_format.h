#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "../definition.h"
#include "../registry/console_entry.h"

namespace sunrise::core::console::output {

/**
 * Bytes one formatted value may occupy.
 *
 * Text is the widest domain and a value carries at most `kTextCapacity` of it, so this holds the
 * longest one with room for the quotes a printed text value keeps.
 */
inline constexpr std::size_t kFormattedValueCapacity = kTextCapacity + 4;

/**
 * Prints one typed value the way a reader types it back.
 *
 * Round-tripping is the point: what this prints must read back through `parse_value` as the same
 * value, so a reader can copy a printed number straight onto the next line. Booleans use
 * `true`/`false`; reals use enough significant digits for an exact round trip, including exponent
 * notation for magnitudes that need it.
 *
 * @param value Value to print.
 * @param buffer Destination, cleared first.
 * @param length Receives the written length.
 */
void format_value(const Value& value, std::span<char> buffer, std::size_t& length) noexcept;

/**
 * Names one outcome for a reader.
 * @param status Outcome to name.
 * @return A sentence fragment naming the cause, empty for `ok`.
 */
[[nodiscard]] constexpr std::string_view status_text(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "";
    case Status::unknownName:
        return "no such name";
    case Status::wrongArgumentCount:
        return "wrong number of arguments";
    case Status::badArgument:
        return "argument is not of the declared type";
    case Status::outOfRange:
        return "argument is outside the declared range";
    case Status::refused:
        return "the module declined";
    case Status::failed:
        return "the command failed";
    }
    return "";
}

/**
 * Prints the one-line usage of an entry, as help and a wrong-argument report both show it.
 * @param entry Entry to describe.
 * @param buffer Destination, cleared first.
 * @param length Receives the written length.
 */
void format_usage(const registry::Descriptor& entry,
                  std::span<char> buffer,
                  std::size_t& length) noexcept;

// Every outcome a reader can cause has to name its own cause, or a failed line would print blank.
static_assert(!status_text(Status::unknownName).empty());
static_assert(!status_text(Status::wrongArgumentCount).empty());
static_assert(!status_text(Status::badArgument).empty());
static_assert(!status_text(Status::outOfRange).empty());
static_assert(!status_text(Status::refused).empty());
static_assert(!status_text(Status::failed).empty());
// Success is reported by the answer itself, so it deliberately has no cause text.
static_assert(status_text(Status::ok).empty());

} // namespace sunrise::core::console::output
