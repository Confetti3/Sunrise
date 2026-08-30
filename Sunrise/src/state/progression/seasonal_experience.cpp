#include "seasonal_experience.h"

#include <Windows.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "season_pass_reward_catalog.h"

namespace sunrise::state::progression::seasonal_experience {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\cache\\seasonal_experience.bin";
constexpr std::array<char, 8> kLegacyMagic{'S', 'N', 'R', 'S', 'X', 'P', '0', '1'};
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'X', 'P', '0', '2'};
constexpr std::size_t kRewardCount = 196;
constexpr std::size_t kRewardClaimByteCount = (kRewardCount + 7U) / 8U;
constexpr std::int32_t kExperiencePerRank = 100'000;
constexpr std::uint16_t kMaximumRank = 100;

std::mutex g_lock;
std::int32_t g_experience{};
std::array<std::uint8_t, kRewardClaimByteCount> g_rewardClaims{};
core::path::Buffer g_path{};
bool g_pathReady{};

void store_locked() noexcept {
    if (!g_pathReady) {
        return;
    }
    std::array<std::byte,
               kMagic.size() + sizeof(g_experience) + kRewardClaimByteCount>
        document{};
    std::memcpy(document.data(), kMagic.data(), kMagic.size());
    std::memcpy(document.data() + kMagic.size(), &g_experience, sizeof g_experience);
    std::memcpy(document.data() + kMagic.size() + sizeof g_experience,
                g_rewardClaims.data(),
                g_rewardClaims.size());
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    (void)WriteFile(file,
                    document.data(),
                    static_cast<DWORD>(document.size()),
                    &written,
                    nullptr);
    (void)CloseHandle(file);
}

void load_locked() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::array<std::byte,
               kMagic.size() + sizeof(g_experience) + kRewardClaimByteCount>
        document{};
    DWORD read = 0;
    const bool readable = ReadFile(file,
                                   document.data(),
                                   static_cast<DWORD>(document.size()),
                                   &read,
                                   nullptr)
                          != FALSE;
    (void)CloseHandle(file);
    std::int32_t restored = 0;
    const std::size_t legacySize = kLegacyMagic.size() + sizeof restored;
    const bool current = readable && read == document.size()
                         && std::memcmp(document.data(), kMagic.data(), kMagic.size()) == 0;
    const bool legacy = readable && read == legacySize
                        && std::memcmp(document.data(), kLegacyMagic.data(), kLegacyMagic.size())
                               == 0;
    if (current || legacy) {
        std::memcpy(&restored, document.data() + kMagic.size(), sizeof restored);
    }
    if (restored >= 0) {
        g_experience = restored;
    }
    if (current) {
        std::memcpy(g_rewardClaims.data(),
                    document.data() + kMagic.size() + sizeof g_experience,
                    g_rewardClaims.size());
    }
}

} // namespace

bool initialize(void* module) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_experience = 0;
    g_rewardClaims.fill(0);
    g_pathReady = core::path::artifact_directory(module, g_path)
                  && core::path::append(g_path, kFileSuffix);
    if (g_pathReady) {
        load_locked();
    }
    return g_pathReady;
}

void shutdown() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_experience = 0;
    g_rewardClaims.fill(0);
    g_path = {};
    g_pathReady = false;
}

bool grant(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    if (g_experience > (std::numeric_limits<std::int32_t>::max)() - amount) {
        return false;
    }
    g_experience += amount;
    store_locked();
    return true;
}

std::int32_t earned() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_experience;
}

std::uint16_t rank() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::int32_t earnedRanks = g_experience / kExperiencePerRank;
    return static_cast<std::uint16_t>(
        (std::min)(static_cast<std::int32_t>(kMaximumRank), earnedRanks + 1));
}

bool reward_claimed(std::uint16_t rewardIndex) noexcept {
    if (rewardIndex >= kRewardCount) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << (rewardIndex & 7U));
    return (g_rewardClaims[rewardIndex >> 3U] & mask) != 0;
}

bool claim_reward(std::uint16_t rewardIndex) noexcept {
    if (rewardIndex >= kRewardCount) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::uint8_t mask = static_cast<std::uint8_t>(1U << (rewardIndex & 7U));
    std::uint8_t& byte = g_rewardClaims[rewardIndex >> 3U];
    if ((byte & mask) != 0) {
        return false;
    }
    byte = static_cast<std::uint8_t>(byte | mask);
    store_locked();
    return true;
}

bool apply_reward_claims(std::span<std::uint8_t> acquiredFlags) noexcept {
    constexpr std::uint8_t kAcquiredFlagValue = 2;
    if (acquiredFlags.size()
        <= season_pass::claim_account_flag_index(
            static_cast<std::uint16_t>(season_pass::kRewards.size() - 1U))) {
        return false;
    }

    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::uint16_t rewardIndex = 0; rewardIndex < season_pass::kRewards.size(); ++rewardIndex) {
        const std::uint8_t mask = static_cast<std::uint8_t>(1U << (rewardIndex & 7U));
        if ((g_rewardClaims[rewardIndex >> 3U] & mask) == 0) {
            continue;
        }
        acquiredFlags[season_pass::claim_account_flag_index(rewardIndex)] = kAcquiredFlagValue;
    }
    return true;
}

} // namespace sunrise::state::progression::seasonal_experience
