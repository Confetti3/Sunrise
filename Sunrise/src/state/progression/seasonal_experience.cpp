#include "seasonal_experience.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#include "../../core/filesystem/path.h"

namespace sunrise::state::progression::seasonal_experience {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\cache\\seasonal_experience.bin";
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'X', 'P', '0', '1'};

std::mutex g_lock;
std::int32_t g_experience{};
core::path::Buffer g_path{};
bool g_pathReady{};

void store_locked() noexcept {
    if (!g_pathReady) {
        return;
    }
    std::array<std::byte, kMagic.size() + sizeof(g_experience)> document{};
    std::memcpy(document.data(), kMagic.data(), kMagic.size());
    std::memcpy(document.data() + kMagic.size(), &g_experience, sizeof g_experience);
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
    std::array<std::byte, kMagic.size() + sizeof(g_experience)> document{};
    DWORD read = 0;
    const bool complete =
        ReadFile(file,
                 document.data(),
                 static_cast<DWORD>(document.size()),
                 &read,
                 nullptr)
            != FALSE
        && read == document.size()
        && std::memcmp(document.data(), kMagic.data(), kMagic.size()) == 0;
    (void)CloseHandle(file);
    std::int32_t restored = 0;
    if (complete) {
        std::memcpy(&restored, document.data() + kMagic.size(), sizeof restored);
    }
    if (restored >= 0) {
        g_experience = restored;
    }
}

} // namespace

bool initialize(void* module) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_experience = 0;
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

} // namespace sunrise::state::progression::seasonal_experience
