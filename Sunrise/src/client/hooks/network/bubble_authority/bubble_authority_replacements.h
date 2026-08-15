#pragma once

#include <cstdint>

namespace sunrise::client::hooks::network::bubble_authority {

/** Copied, pointer-free placed-content authority counters. */
struct AuthorityObservation final {
    std::uint64_t decodeCount;
    std::uint64_t forcedReadCount;
    std::uint64_t lastDecoderForcedReads;
    std::uint64_t droppedCount;
    bool lastDecoderSucceeded;
};

/**
 * Copies the current placed-content authority snapshot without blocking.
 * @return False only when a decoder result is being recorded; retry on a later service slice.
 */
[[nodiscard]] bool try_observation(AuthorityObservation& output) noexcept;

/** @return The roster-prefix decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/** @return The content-untracked getter replacement body. */
[[nodiscard]] void* content_untracked_entry_point() noexcept;

} // namespace sunrise::client::hooks::network::bubble_authority
