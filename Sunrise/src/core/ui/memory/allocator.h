#pragma once

#include <cstddef>

namespace sunrise::core::ui::memory {

/** 8 MiB is the first-fit arena for Dear ImGui context, font, widget, and draw storage. */
inline constexpr std::size_t kArenaCapacityBytes = 8'388'608;

/** Copied allocator counters. The arena storage itself is not exposed. */
struct Stats {
    std::size_t capacityBytes{kArenaCapacityBytes};
    std::size_t outstandingBytes{};
    std::size_t largestFreeBytes{};
    std::size_t arenaMisses{};
    std::size_t spillOutstandingAllocations{};
    std::size_t spillOutstandingBytes{};
    std::size_t spillHighWaterBytes{};
    std::size_t allocationFailures{};
    std::size_t lastFailedBytes{};
};

/**
 * Installs the fixed allocator before any Dear ImGui context exists.
 * @return True when the allocator is installed or was already installed.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Restores the earlier Dear ImGui allocator once every owned allocation is freed.
 * @return False while an allocation is still live; the allocator stays active.
 */
[[nodiscard]] bool shutdown() noexcept;

/** @return One copy of the allocator counters, read under the lock. */
[[nodiscard]] Stats snapshot() noexcept;

} // namespace sunrise::core::ui::memory
