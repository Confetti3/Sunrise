#include "console_format.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>

namespace sunrise::core::console::output {
namespace {

/** Appends what fits of one run, advancing the length. */
void append(std::span<char> buffer, std::size_t& length, std::string_view text) noexcept {
    for (const char character : text) {
        if (length + 1 >= buffer.size()) {
            return;
        }
        buffer[length] = character;
        ++length;
    }
}

/** @return True when printed text needs quotes to read back as one token. */
[[nodiscard]] bool needs_quotes(std::string_view text) noexcept {
    if (text.empty()) {
        return true;
    }
    for (const char character : text) {
        if (character == ' ' || character == '"' || character == '\\') {
            return true;
        }
    }
    return false;
}

} // namespace

/** Prints one typed value the way a reader types it back. */
void format_value(const Value& value, std::span<char> buffer, std::size_t& length) noexcept {
    length = 0;
    if (buffer.empty()) {
        return;
    }
    for (char& character : buffer) {
        character = '\0';
    }

    switch (value.type) {
    case Type::boolean:
        append(buffer, length, value.boolean ? "true" : "false");
        return;
    case Type::integer: {
        std::array<char, 32> digits{};
        const int written = std::snprintf(
            digits.data(), digits.size(), "%lld", static_cast<long long>(value.integer));
        if (written > 0) {
            const std::size_t available = digits.size() - 1;
            const std::size_t stored = (std::min)(static_cast<std::size_t>(written), available);
            append(buffer, length, std::string_view{digits.data(), stored});
        }
        return;
    }
    case Type::real: {
        if (!std::isfinite(value.real)) {
            append(buffer, length, "<invalid>");
            return;
        }
        std::array<char, 64> digits{};
        const auto converted = std::to_chars(digits.data(),
                                             digits.data() + digits.size(),
                                             value.real,
                                             std::chars_format::general,
                                             std::numeric_limits<double>::max_digits10);
        if (converted.ec == std::errc{}) {
            append(buffer,
                   length,
                   std::string_view{digits.data(),
                                    static_cast<std::size_t>(converted.ptr - digits.data())});
        } else {
            append(buffer, length, "<invalid>");
        }
        return;
    }
    case Type::text: {
        const std::size_t safeLength = (std::min)(value.textLength, value.text.size());
        const std::string_view text{value.text.data(), safeLength};
        if (!needs_quotes(text)) {
            append(buffer, length, text);
            return;
        }
        append(buffer, length, "\"");
        for (const char character : text) {
            if (character == '\\' || character == '"') {
                append(buffer, length, "\\");
            }
            append(buffer, length, std::string_view{&character, 1});
        }
        append(buffer, length, "\"");
        return;
    }
    case Type::count:
        break;
    }
}

/** Prints the one-line usage of an entry. */
void format_usage(const registry::Descriptor& entry,
                  std::span<char> buffer,
                  std::size_t& length) noexcept {
    length = 0;
    if (buffer.empty()) {
        return;
    }
    for (char& character : buffer) {
        character = '\0';
    }

    append(buffer, length, entry.name);
    if (entry.kind == registry::Kind::variable) {
        // A variable is shown with the value it would take, since typing the name alone reads it.
        append(buffer, length, " [");
        append(buffer, length, type_name(entry.type));
        append(buffer, length, "]");
        if (has_bounds(entry)) {
            std::array<char, 64> range{};
            const int written =
                std::snprintf(range.data(), range.size(), " %g..%g", entry.minimum, entry.maximum);
            if (written > 0) {
                const std::size_t available = range.size() - 1;
                const std::size_t stored =
                    (std::min)(static_cast<std::size_t>(written), available);
                append(buffer, length, std::string_view{range.data(), stored});
            }
        }
        return;
    }
    for (const registry::Argument& argument : entry.arguments) {
        append(buffer, length, " <");
        append(buffer, length, argument.name);
        append(buffer, length, ":");
        append(buffer, length, type_name(argument.type));
        append(buffer, length, ">");
    }
}

} // namespace sunrise::core::console::output
