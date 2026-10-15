#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::state::build_data::scriptables::inline_name_evidence {

/** FNV-1 32-bit offset basis and prime. The packages hash names with FNV-1, not FNV-1a. */
inline constexpr std::uint32_t kHashBasis = 0x811C9DC5U;
inline constexpr std::uint32_t kHashPrime = 0x01000193U;

/** @return The authored 32-bit FNV-1 hash of one exact byte string. */
[[nodiscard]] inline std::uint32_t hash(std::span<const std::byte> bytes) noexcept {
    std::uint32_t output = kHashBasis;
    for (const std::byte byte : bytes) {
        output = (output * kHashPrime) ^ std::to_integer<std::uint8_t>(byte);
    }
    return output;
}

/** @return The authored 32-bit FNV-1 hash of one exact name. */
[[nodiscard]] constexpr std::uint32_t hash(std::string_view text) noexcept {
    std::uint32_t output = kHashBasis;
    for (const char character : text) {
        output = (output * kHashPrime) ^ static_cast<std::uint8_t>(character);
    }
    return output;
}

/** @return True when the whole nonempty byte string is canonical UTF-8. */
[[nodiscard]] inline bool valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const std::uint8_t first = std::to_integer<std::uint8_t>(bytes[cursor]);
        if (first <= 0x7FU) {
            ++cursor;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuationCount = 1;
            codePoint = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuationCount = 2;
            codePoint = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuationCount = 3;
            codePoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuationCount > bytes.size() - cursor - 1) {
            return false;
        }
        for (std::size_t index = 0; index < continuationCount; ++index) {
            const std::uint8_t next = std::to_integer<std::uint8_t>(bytes[cursor + index + 1]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }
        if (codePoint < minimum || codePoint > 0x10FFFFU
            || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
            return false;
        }
        cursor += continuationCount + 1;
    }
    return !bytes.empty();
}

} // namespace sunrise::state::build_data::scriptables::inline_name_evidence
