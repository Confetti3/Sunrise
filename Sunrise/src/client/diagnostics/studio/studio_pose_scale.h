#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace sunrise::client::diagnostics::studio {

/** Cached Z and scale use the same native XOR mask; decoded Z retains its exact bits. */
[[nodiscard]] inline float decode_composed_scale(const std::array<std::uint32_t, 4>& masked,
                                                 float decodedZ) noexcept {
    return std::bit_cast<float>(masked[3] ^ masked[2] ^ std::bit_cast<std::uint32_t>(decodedZ));
}

} // namespace sunrise::client::diagnostics::studio
