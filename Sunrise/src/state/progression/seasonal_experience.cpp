#include "seasonal_experience.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../unlocks/definition.h"
#include "season_pass_reward_catalog.h"

namespace sunrise::state::progression::seasonal_experience {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\cache\\seasonal_experience.bin";
constexpr std::wstring_view kTemporarySuffix = L".tmp";
constexpr std::array<char, 8> kLegacyMagic{'S', 'N', 'R', 'S', 'X', 'P', '0', '1'};
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'X', 'P', '0', '2'};
constexpr std::size_t kRewardCount = season_pass::kRewards.size();
constexpr std::size_t kRewardClaimByteCount = (kRewardCount + 7U) / 8U;
constexpr std::size_t kLegacyDocumentSize = kLegacyMagic.size() + sizeof(std::int32_t);
constexpr std::size_t kDocumentSize = kLegacyDocumentSize + kRewardClaimByteCount;
constexpr std::int32_t kExperiencePerRank = 100'000;
constexpr std::uint16_t kMaximumRank = 100;

std::mutex g_lock;
std::int32_t g_experience{};
std::array<std::uint8_t, kRewardClaimByteCount> g_rewardClaims{};
core::path::Buffer g_path{};
bool g_pathReady{};
bool g_persistenceRequired{};

[[nodiscard]] constexpr std::size_t reward_claim_byte(std::uint16_t rewardIndex) noexcept {
    return rewardIndex >> 3U;
}

[[nodiscard]] constexpr std::uint8_t reward_claim_mask(std::uint16_t rewardIndex) noexcept {
    return static_cast<std::uint8_t>(1U << (rewardIndex & 7U));
}

[[nodiscard]] bool store_locked() noexcept {
    if (!g_pathReady) {
        return !g_persistenceRequired;
    }
    std::array<std::byte, kDocumentSize> document{};
    std::memcpy(document.data(), kMagic.data(), kMagic.size());
    std::memcpy(document.data() + kMagic.size(), &g_experience, sizeof g_experience);
    std::memcpy(document.data() + kMagic.size() + sizeof g_experience,
                g_rewardClaims.data(),
                g_rewardClaims.size());
    core::path::Buffer temporaryPath = g_path;
    if (!core::path::append(temporaryPath, kTemporarySuffix)) {
        return false;
    }
    const HANDLE file = CreateFileW(temporaryPath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool stored =
        WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written, nullptr)
            != FALSE
        && written == document.size() && FlushFileBuffers(file) != FALSE;
    stored = CloseHandle(file) != FALSE && stored;
    stored = stored
             && MoveFileExW(temporaryPath.chars.data(),
                            g_path.chars.data(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                    != FALSE;
    if (!stored) {
        (void)DeleteFileW(temporaryPath.chars.data());
    }
    return stored;
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
    LARGE_INTEGER fileSize{};
    const bool sized = GetFileSizeEx(file, &fileSize) != FALSE && fileSize.QuadPart >= 0;
    std::array<std::byte, kDocumentSize> document{};
    DWORD read = 0;
    const bool readable =
        ReadFile(file, document.data(), static_cast<DWORD>(document.size()), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    std::int32_t restored = 0;
    const bool current = sized && fileSize.QuadPart == document.size() && readable
                         && read == document.size()
                         && std::memcmp(document.data(), kMagic.data(), kMagic.size()) == 0;
    const bool legacy =
        sized && fileSize.QuadPart == kLegacyDocumentSize && readable && read == kLegacyDocumentSize
        && std::memcmp(document.data(), kLegacyMagic.data(), kLegacyMagic.size()) == 0;
    if (!current && !legacy) {
        return;
    }
    std::memcpy(&restored, document.data() + kMagic.size(), sizeof restored);
    if (restored < 0) {
        return;
    }
    g_experience = restored;
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
    g_path = {};
    g_pathReady = false;
    g_persistenceRequired = module != nullptr;
    if (!g_persistenceRequired) {
        return true;
    }
    if (!core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kFileSuffix)) {
        return false;
    }
    g_pathReady = true;
    load_locked();
    return true;
}

void shutdown() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_experience = 0;
    g_rewardClaims.fill(0);
    g_path = {};
    g_pathReady = false;
    g_persistenceRequired = false;
}

bool grant(std::int32_t amount) noexcept {
    if (amount <= 0) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    if (g_experience > (std::numeric_limits<std::int32_t>::max)() - amount) {
        return false;
    }
    const std::int32_t previous = g_experience;
    g_experience += amount;
    if (store_locked()) {
        return true;
    }
    g_experience = previous;
    return false;
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
    return (g_rewardClaims[reward_claim_byte(rewardIndex)] & reward_claim_mask(rewardIndex)) != 0;
}

bool claim_reward(std::uint16_t rewardIndex) noexcept {
    if (rewardIndex >= kRewardCount) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    const std::uint8_t mask = reward_claim_mask(rewardIndex);
    std::uint8_t& byte = g_rewardClaims[reward_claim_byte(rewardIndex)];
    if ((byte & mask) != 0) {
        return false;
    }
    const std::uint8_t previous = byte;
    byte = static_cast<std::uint8_t>(byte | mask);
    if (store_locked()) {
        return true;
    }
    byte = previous;
    return false;
}

bool apply_reward_claims(std::span<std::uint8_t> acquiredFlags) noexcept {
    if (acquiredFlags.size() <= season_pass::claim_account_flag_index(
            static_cast<std::uint16_t>(season_pass::kRewards.size() - 1U))) {
        return false;
    }

    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::size_t index = 0; index < kRewardCount; ++index) {
        const auto rewardIndex = static_cast<std::uint16_t>(index);
        if ((g_rewardClaims[reward_claim_byte(rewardIndex)] & reward_claim_mask(rewardIndex))
            == 0) {
            continue;
        }
        acquiredFlags[season_pass::claim_account_flag_index(rewardIndex)] = unlocks::kFlagSet;
    }
    return true;
}

} // namespace sunrise::state::progression::seasonal_experience
