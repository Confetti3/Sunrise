#include "record_claims.h"

#include <array>
#include <bit>
#include <mutex>

#include "../unlocks/definition.h"

namespace sunrise::state::record_claims {
namespace {

/** One bit per addressable account flag index, which is cheaper than a set and never allocates. */
constexpr std::size_t kIndexCapacity = unlocks::kAccountFlagCapacity;
constexpr std::size_t kWordBits = 64;
constexpr std::size_t kWordCount = (kIndexCapacity + kWordBits - 1) / kWordBits;

std::mutex g_lock;
std::array<std::uint64_t, kWordCount> g_claimed{};
std::size_t g_count{};
std::uint32_t g_score{};

} // namespace

/** Forgets every claim made since boot. */
void clear() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_claimed.fill(0);
    g_count = 0;
    g_score = 0;
}

/** Marks one account flag bank index claimed. */
bool claim(std::uint16_t flagIndex, std::uint16_t scoreValue) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(flagIndex) / kWordBits;
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
    const std::lock_guard<std::mutex> guard(g_lock);
    if ((g_claimed[word] & bit) == 0) {
        g_claimed[word] |= bit;
        ++g_count;
        // Only a first claim scores, so a repeated click cannot inflate the total.
        g_score += scoreValue;
    }
    return true;
}

/** Lays every held claim over one account flag bank. */
std::size_t apply(std::span<std::uint8_t> accountFlags) noexcept {
    std::size_t changed = 0;
    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::size_t word = 0; word < g_claimed.size(); ++word) {
        std::uint64_t bits = g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const std::size_t index = word * kWordBits + offset;
            // A bank shorter than the index space is not an error: the tail simply is not sent.
            if (index >= accountFlags.size()) {
                continue;
            }
            if (accountFlags[index] != unlocks::kFlagSet) {
                accountFlags[index] = unlocks::kFlagSet;
                ++changed;
            }
        }
    }
    return changed;
}

/** @return Total score of every record claimed since boot. */
std::uint32_t total_score() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_score;
}

/** @return Number of distinct indices claimed since boot. */
std::size_t count() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_count;
}

} // namespace sunrise::state::record_claims
