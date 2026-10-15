#include "async_io_lifetime_guard.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../patterns/image_scan.h"
#include "../../process/freeze/client_process_freeze.h"

namespace sunrise::client::hooks::async_io {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Native async-I/O manager wrapper, from its prologue through the post-pump tail.
 * Its null test on the cached manager separates it from two wrappers with the same tail.
 */
constexpr std::string_view kTailText =
    "40 53 48 83 EC 20 48 8B 1D ? ? ? ? 48 85 DB 74 ? 80 BB 61 6C 04 00 00 75 ? 8B 0B 33 D2 "
    "E8 ? ? ? ? C6 83 62 6C 04 00 01 48 8B 1D ? ? ? ? 48 8B CB E8 ? ? ? ? 48 8B 05 ? ? ? ? "
    "80 B8 62 6C 04 00 00 74 ? 8B 08 33 D2 C6 80 62 6C 04 00 00";
/** Compiled form of the pattern above; the scan requires one match. */
constexpr auto kTail = signature<signature_length(kTailText)>(kTailText);

/** The post-pump global reload begins after the second MOV RBX, MOV RCX, and CALL. */
constexpr std::size_t kReloadOffset = 58;
/** `mov rax, rbx` followed by one four-byte architectural NOP. */
constexpr std::array<std::byte, 7> kCachedOwner{std::byte{0x48},
                                                std::byte{0x89},
                                                std::byte{0xD8},
                                                std::byte{0x0F},
                                                std::byte{0x1F},
                                                std::byte{0x40},
                                                std::byte{0x00}};
/** Freeze retries before the patch is abandoned; a thread inside the bytes blocks the write. */
constexpr std::size_t kHoldAttempts = 8;

std::byte* g_reload{};
std::array<std::byte, kCachedOwner.size()> g_original{};
bool g_installed{};

/** @return True when a suspended thread would resume inside the bytes being replaced. */
[[nodiscard]] bool thread_inside_patch(const process::freeze::Held& held,
                                       const std::byte* start) noexcept {
    const auto low = reinterpret_cast<std::uintptr_t>(start);
    const auto high = low + kCachedOwner.size();
    for (std::size_t index = 0; index < held.count; ++index) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(held.handles[index], &context) == FALSE) {
            return true;
        }
        const auto instruction = static_cast<std::uintptr_t>(context.Rip);
        if (instruction >= low && instruction < high) {
            return true;
        }
    }
    return false;
}

/** Suspends the process only when no native thread stopped in the patch window. */
[[nodiscard]] bool hold_away_from_patch(process::freeze::Held& held,
                                        const std::byte* start) noexcept {
    for (std::size_t attempt = 0; attempt < kHoldAttempts; ++attempt) {
        if (!process::freeze::hold(held)) {
            return false;
        }
        if (!thread_inside_patch(held, start)) {
            return true;
        }
        process::freeze::release(held);
        Sleep(1);
    }
    return false;
}

/** Writes one complete instruction replacement while every competing process thread is held. */
template <std::size_t Size>
[[nodiscard]] bool write_code(std::byte* destination,
                              const std::array<std::byte, Size>& bytes) noexcept {
    process::freeze::Held held{};
    if (destination == nullptr || !hold_away_from_patch(held, destination)) {
        return false;
    }
    DWORD originalProtection = 0;
    bool written =
        VirtualProtect(destination, bytes.size(), PAGE_EXECUTE_READWRITE, &originalProtection)
        != FALSE;
    if (written) {
        std::memcpy(destination, bytes.data(), bytes.size());
        written = FlushInstructionCache(GetCurrentProcess(), destination, bytes.size()) != FALSE;
        DWORD ignored = 0;
        written = VirtualProtect(destination, bytes.size(), originalProtection, &ignored) != FALSE
                  && written;
    }
    process::freeze::release(held);
    return written;
}

} // namespace

/** Installs the native async-manager cached-owner guard when this game build matches. */
bool install() noexcept {
    if (g_installed) {
        return true;
    }
    std::byte* const tail = scan_main_image_unique(kTail, "async_io_manager_post_pump_tail");
    if (tail == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=async_io_guard stage=resolve result=fail");
        return false;
    }
    g_reload = tail + kReloadOffset;
    std::memcpy(g_original.data(), g_reload, g_original.size());
    const bool patchWriteComplete = write_code(g_reload, kCachedOwner);
    if (!patchWriteComplete
        && std::memcmp(g_reload, kCachedOwner.data(), kCachedOwner.size()) != 0) {
        g_reload = nullptr;
        g_original = {};
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=async_io_guard stage=patch result=fail");
        return false;
    }
    g_installed = true;
    core::log::write(core::log::Channel::client,
                     patchWriteComplete ? core::log::Level::info : core::log::Level::warn,
                     "ev=async_io_guard stage=patch result=ok mode=cached_manager_owner");
    return true;
}

/** Restores the exact original instruction bytes when this module installed the guard. */
bool uninstall() noexcept {
    if (!g_installed) {
        return true;
    }
    const bool restoreComplete = write_code(g_reload, g_original);
    if (!restoreComplete && std::memcmp(g_reload, g_original.data(), g_original.size()) != 0) {
        return false;
    }
    g_reload = nullptr;
    g_original = {};
    g_installed = false;
    return true;
}

} // namespace sunrise::client::hooks::async_io
