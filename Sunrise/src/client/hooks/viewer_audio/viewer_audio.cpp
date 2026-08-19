#include "viewer_audio.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../viewer_camera/viewer_camera.h"

namespace sunrise::client::viewer::audio {
namespace {

constexpr std::string_view kListenerPositionText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B F2 48 8B E9 "
    "E8 ? ? ? ? 48 8B 0D ? ? ? ? BA 10 00 00 00 44 0F B7 C0 E8 ? ? ? ?";
constexpr auto kListenerPosition =
    patterns::signature<patterns::signature_length(kListenerPositionText)>(kListenerPositionText);
constexpr std::int32_t kPrimaryListener = 0;

struct WwiseVector {
    float x{};
    float y{};
    float z{};
};

/** Shadowkeep's legacy Wwise order, proved by the listener wrapper and SDK headers. */
struct ListenerPosition {
    WwiseVector front{};
    WwiseVector top{};
    WwiseVector position{};
};
static_assert(sizeof(ListenerPosition) == 36);

using ListenerUpdate = std::int64_t(__fastcall*)(const ListenerPosition*, std::int32_t);

hooking::detour::Handle g_handle{};
std::atomic<ListenerUpdate> g_original{};
std::atomic_bool g_installPublishing{};
std::atomic_bool g_installed{};
std::atomic_bool g_stopping{};
std::atomic_bool g_applied{};
std::atomic_bool g_wasActive{};
SRWLOCK g_nativeLock{SRWLOCK_INIT};
ListenerPosition g_nativePosition{};
std::atomic_bool g_nativePresent{};
std::atomic_uint g_replacementInFlight{};

struct ReplacementScope {
    ReplacementScope() noexcept {
        g_replacementInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ReplacementScope() {
        g_replacementInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
    ReplacementScope(const ReplacementScope&) = delete;
    ReplacementScope& operator=(const ReplacementScope&) = delete;
};

[[nodiscard]] bool replacement_idle() noexcept {
    return g_replacementInFlight.load(std::memory_order_acquire) == 0;
}

void report(const char* stage, const char* result, const char* reason = nullptr) noexcept {
    std::array<char, 128> line{};
    const int written =
        reason == nullptr
            ? std::snprintf(
                  line.data(), line.size(), "ev=viewer_audio stage=%s result=%s", stage, result)
            : std::snprintf(line.data(),
                            line.size(),
                            "ev=viewer_audio stage=%s result=%s reason=%s",
                            stage,
                            result,
                            reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] ListenerUpdate original() noexcept {
    ListenerUpdate next = g_original.load(std::memory_order_acquire);
    while (next == nullptr && g_installPublishing.load(std::memory_order_acquire)) {
        SwitchToThread();
        next = g_original.load(std::memory_order_acquire);
    }
    return next;
}

[[nodiscard]] bool finite(const camera::Vector& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

/** Destiny X-forward/Z-up to Wwise X-left/Y-up/Z-forward. */
[[nodiscard]] WwiseVector convert(const camera::Vector& value) noexcept {
    return WwiseVector{-value[1], value[2], value[0]};
}

void publish_native(const ListenerPosition& position) noexcept {
    AcquireSRWLockExclusive(&g_nativeLock);
    g_nativePosition = position;
    g_nativePresent.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_nativeLock);
}

[[nodiscard]] bool native_snapshot(ListenerPosition& position) noexcept {
    AcquireSRWLockShared(&g_nativeLock);
    const bool present = g_nativePresent.load(std::memory_order_acquire);
    if (present) {
        position = g_nativePosition;
    }
    ReleaseSRWLockShared(&g_nativeLock);
    return present;
}

[[nodiscard]] bool viewer_position(ListenerPosition& position) noexcept {
    camera::Pose pose{};
    if (!camera::pose_snapshot(pose) || !finite(pose.position) || !finite(pose.forward)
        || !finite(pose.up)) {
        return false;
    }
    position.front = convert(pose.forward);
    position.top = convert(pose.up);
    position.position = convert(pose.position);
    return true;
}

std::int64_t __fastcall update_listener(const ListenerPosition* position,
                                        std::int32_t listenerIndex) noexcept {
    ReplacementScope scope{};
    const ListenerUpdate next = original();
    if (next == nullptr) {
        return 2;
    }
    if (listenerIndex == kPrimaryListener && position != nullptr) {
        publish_native(*position);
    }
    if (g_stopping.load(std::memory_order_acquire) || listenerIndex != kPrimaryListener) {
        return next(position, listenerIndex);
    }

    ListenerPosition redirected{};
    if (!viewer_position(redirected)) {
        return next(position, listenerIndex);
    }
    const std::int64_t result = next(&redirected, listenerIndex);
    g_wasActive.store(true, std::memory_order_release);
    g_applied.store(result == 1, std::memory_order_release);
    return result;
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    g_stopping.store(false, std::memory_order_release);
    g_wasActive.store(false, std::memory_order_release);
    g_applied.store(false, std::memory_order_release);
    g_nativePresent.store(false, std::memory_order_release);
    g_installPublishing.store(true, std::memory_order_release);
    std::byte* const target =
        patterns::scan_main_image_unique(kListenerPosition, "viewer_audio_listener_position");
    if (target == nullptr) {
        g_installPublishing.store(false, std::memory_order_release);
        report("install", "fail", "listener_signature");
        return false;
    }
    if (!hooking::detour::install(
            hooking::detour::Spec{target, reinterpret_cast<void*>(&update_listener)}, g_handle)) {
        g_installPublishing.store(false, std::memory_order_release);
        report("install", "fail", "detour_attach");
        return false;
    }
    g_original.store(reinterpret_cast<ListenerUpdate>(g_handle.original),
                     std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    g_installPublishing.store(false, std::memory_order_release);
    report("install", "ok");
    return true;
}

void apply() noexcept {
    ReplacementScope scope{};
    if (!g_installed.load(std::memory_order_acquire)
        || g_stopping.load(std::memory_order_acquire)) {
        return;
    }
    const ListenerUpdate next = original();
    if (next == nullptr) {
        return;
    }

    ListenerPosition position{};
    if (viewer_position(position)) {
        const std::int64_t result = next(&position, kPrimaryListener);
        g_wasActive.store(true, std::memory_order_release);
        g_applied.store(result == 1, std::memory_order_release);
        return;
    }

    if (g_wasActive.exchange(false, std::memory_order_acq_rel) && native_snapshot(position)) {
        (void)next(&position, kPrimaryListener);
    }
    g_applied.store(false, std::memory_order_release);
}

bool uninstall() noexcept {
    g_stopping.store(true, std::memory_order_release);
    g_applied.store(false, std::memory_order_release);
    if (!replacement_idle()) {
        report("uninstall", "wait", "replacement_active");
        return false;
    }
    ListenerPosition native{};
    const ListenerUpdate next = original();
    if (g_wasActive.exchange(false, std::memory_order_acq_rel) && next != nullptr
        && native_snapshot(native)) {
        (void)next(&native, kPrimaryListener);
    }
    if (g_handle.attached) {
        const std::array protectedEntries{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&update_listener)},
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&apply)},
        };
        const hooking::detour::UninstallResult removal = hooking::detour::uninstall(
            std::span(&g_handle, 1), protectedEntries, &replacement_idle);
        if (removal != hooking::detour::UninstallResult::removed) {
            report("uninstall",
                   removal == hooking::detour::UninstallResult::protectedCodeActive ? "wait"
                                                                                    : "fail",
                   removal == hooking::detour::UninstallResult::protectedCodeActive
                       ? "replacement_active"
                       : "detour_detach");
            return false;
        }
    }
    g_handle = {};
    g_original.store(nullptr, std::memory_order_release);
    g_nativePresent.store(false, std::memory_order_release);
    g_installed.store(false, std::memory_order_release);
    report("uninstall", "ok");
    return true;
}

bool installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

bool applied() noexcept {
    return g_applied.load(std::memory_order_acquire);
}

} // namespace sunrise::client::viewer::audio
