#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "detour_transaction_limits.h"

namespace sunrise::client::hooking::detour::storage {

enum class Kind { thread, operation };
/** The thread node stores two native pointers. */
inline constexpr std::size_t kThreadBytes = 16;
/** Operation slots reserve one cache line; the vendor adapter checks its record size. */
inline constexpr std::size_t kOperationBytes = 64;
/** Every slot supports all fundamental alignments without a runtime allocator. */
inline constexpr std::size_t kAlignment = alignof(std::max_align_t);

namespace detail {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

/** Statically initialized slots use only bounded, lock-free ownership operations. */
template <std::size_t Count, std::size_t Bytes> struct Pool {
    /** One bitmap bit owns one slot; each atomic word provides 64 bits. */
    static constexpr std::size_t kWordBits = std::numeric_limits<std::uint64_t>::digits;
    struct alignas(kAlignment) Slot {
        std::array<std::byte, Bytes> data{};
    };
    std::array<Slot, Count> slots{};
    std::array<std::atomic<std::uint64_t>, (Count + kWordBits - 1) / kWordBits> words{};

    /**
     * Bounded scans return null instead of entering a CRT or process-heap allocator.
     * @param bytes Requested object size, checked before a slot is claimed.
     * @return Aligned storage or null when the request or capacity cannot be satisfied.
     */
    [[nodiscard]] void* acquire(std::size_t bytes) noexcept {
        if (bytes == 0 || bytes > Bytes) {
            return nullptr;
        }
        for (std::size_t word = 0; word < words.size(); ++word) {
            const std::size_t remaining = Count - word * kWordBits;
            const std::uint64_t valid = remaining >= kWordBits
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : (std::uint64_t{1} << remaining) - 1;
            auto observed = words[word].load(std::memory_order_relaxed);
            for (std::size_t attempt = 0; attempt < kWordBits; ++attempt) {
                const auto available = ~observed & valid;
                if (available == 0) {
                    break;
                }
                const auto bit = static_cast<std::size_t>(std::countr_zero(available));
                const auto mask = std::uint64_t{1} << bit;
                if (words[word].compare_exchange_strong(observed,
                                                        observed | mask,
                                                        std::memory_order_acquire,
                                                        std::memory_order_relaxed)) {
                    return slots[word * kWordBits + bit].data.data();
                }
            }
        }
        return nullptr;
    }

    /**
     * Only an occupied slot's exact base address may return to this pool.
     * @param pointer Object storage returned by this pool.
     * @return True for the first release of an occupied slot.
     */
    [[nodiscard]] bool release(void* pointer) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto begin = reinterpret_cast<std::uintptr_t>(slots.data());
        if (address < begin || address - begin >= sizeof(slots)
            || (address - begin) % sizeof(Slot) != 0) {
            return false;
        }
        const auto index = (address - begin) / sizeof(Slot);
        const auto mask = std::uint64_t{1} << (index % kWordBits);
        return (words[index / kWordBits].fetch_and(~mask, std::memory_order_release) & mask) != 0;
    }

    /** @return Occupied slots for bounded transaction validation. */
    [[nodiscard]] std::size_t in_use() const noexcept {
        std::size_t result{};
        for (const auto& word : words) {
            result += static_cast<std::size_t>(std::popcount(word.load(std::memory_order_relaxed)));
        }
        return result;
    }
};

inline constinit Pool<limits::kThreadCount, kThreadBytes> threads{};
inline constinit Pool<limits::kOperationCount, kOperationBytes> operations{};
} // namespace detail

/** @return Fixed storage for one transaction node, or null on invalid size or exhaustion. */
[[nodiscard]] inline void* allocate(Kind kind, std::size_t bytes) noexcept {
    return kind == Kind::thread ? detail::threads.acquire(bytes)
                                : detail::operations.acquire(bytes);
}

/** @return True when the exact occupied node belonged to the selected pool. */
inline bool release(Kind kind, void* pointer) noexcept {
    return kind == Kind::thread ? detail::threads.release(pointer)
                                : detail::operations.release(pointer);
}

/** @return Occupied nodes of the selected kind. */
[[nodiscard]] inline std::size_t in_use(Kind kind) noexcept {
    return kind == Kind::thread ? detail::threads.in_use() : detail::operations.in_use();
}

} // namespace sunrise::client::hooking::detour::storage
