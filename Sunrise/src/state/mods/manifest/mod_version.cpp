#include "mod_version.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace sunrise::state::mods::version {
namespace {

/** A version has exactly three dot-separated parts. */
constexpr std::size_t kPartCount = 3;
/** Parts are decimal. */
constexpr std::uint32_t kDecimalRadix = 10;
/** Each part fits the 16-bit field it is stored in. */
constexpr std::uint32_t kPartMaximum = (std::numeric_limits<std::uint16_t>::max)();

/**
 * Parses one decimal part with no sign and no leading zero.
 * @return False for an empty, padded or oversized part.
 */
[[nodiscard]] bool parse_part(std::string_view text, std::uint16_t& output) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * kDecimalRadix + static_cast<std::uint32_t>(character - '0');
        if (value > kPartMaximum) {
            return false;
        }
    }
    output = static_cast<std::uint16_t>(value);
    return true;
}

} // namespace

/** Parses `MAJOR.MINOR.PATCH`. */
bool parse(std::string_view text, Version& output) noexcept {
    std::array<std::uint16_t, kPartCount> parts{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < kPartCount; ++index) {
        const bool last = index + 1 == kPartCount;
        const std::size_t end = last ? text.size() : text.find('.', start);
        if (end == std::string_view::npos
            || !parse_part(text.substr(start, end - start), parts[index])) {
            return false;
        }
        start = end + 1;
    }
    output = Version{parts[0], parts[1], parts[2]};
    return true;
}

/** @return Negative, zero or positive as `first` is lower, equal or higher. */
int compare(const Version& first, const Version& second) noexcept {
    if (first.major != second.major) {
        return first.major < second.major ? -1 : 1;
    }
    if (first.minor != second.minor) {
        return first.minor < second.minor ? -1 : 1;
    }
    if (first.patch != second.patch) {
        return first.patch < second.patch ? -1 : 1;
    }
    return 0;
}

/** @return True when `value` lies in the dependency's inclusive range. */
bool satisfies(const Dependency& dependency, const Version& value) noexcept {
    return compare(value, dependency.minimum) >= 0
           && (!dependency.hasMaximum || compare(value, dependency.maximum) <= 0);
}

/** @return The null-terminated `MAJOR.MINOR.PATCH` text. */
std::array<char, kTextCapacity> text(const Version& value) noexcept {
    std::array<char, kTextCapacity> output{};
    (void)std::snprintf(output.data(),
                        output.size(),
                        "%u.%u.%u",
                        static_cast<unsigned>(value.major),
                        static_cast<unsigned>(value.minor),
                        static_cast<unsigned>(value.patch));
    return output;
}

} // namespace sunrise::state::mods::version
