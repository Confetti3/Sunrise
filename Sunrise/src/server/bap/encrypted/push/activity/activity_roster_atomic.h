#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace sunrise::server::bap::encrypted::push::activity::detail {

/**
 * Appends one diagnostic frame immediately before one roster and rolls the pair back together.
 * The callbacks keep the helper independent of the authentic codecs and make the byte/nonce
 * transaction directly testable.
 */
template <std::size_t NonceSize, typename AppendEntitySlots, typename AppendRoster,
          typename DiscardRoster>
[[nodiscard]] bool append_entity_slot_roster_pair(
    std::array<std::byte, NonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written,
    AppendEntitySlots&& appendEntitySlots,
    AppendRoster&& appendRoster,
    DiscardRoster&& discardRoster) noexcept {
    if (written > response.size()) return false;
    const std::size_t initialWritten = written;
    const auto initialNonce = nonce;
    if (appendEntitySlots() && appendRoster()) return true;
    discardRoster();
    if (written > initialWritten && initialWritten < response.size()) {
        const std::size_t clearSize =
            (std::min)(written - initialWritten, response.size() - initialWritten);
        std::fill_n(response.begin() + initialWritten, clearSize, std::byte{});
    }
    written = initialWritten;
    nonce = initialNonce;
    return false;
}

} // namespace sunrise::server::bap::encrypted::push::activity::detail
