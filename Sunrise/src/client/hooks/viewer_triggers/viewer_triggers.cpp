#include "viewer_triggers.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::viewer::triggers {
namespace {

constexpr std::string_view kTriggerTickText =
    "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 80 BB 9F 00 00 00 00 74 1A 80 BB 9E 00 00 00 00 "
    "74 09 0F B6 83 9C 00 00 00 EB 02 B0 01 88 83 A4 00 00 00";
constexpr auto kTriggerTick =
    patterns::signature<patterns::signature_length(kTriggerTickText)>(kTriggerTickText);
constexpr std::string_view kTriggerVolumePostSimulationText =
    "48 8B C4 55 48 8B EC 48 83 EC 70 44 8B 49 18 48 89 58 10 48 89 78 E8 33 FF 4C 89 60 E0 "
    "4C 89 68 D8 4C 89 70 D0 4C 89 78 C8 4C 8B F9 48 89 7D C0 89 7D C8 C7 45 CC 00 00 00 80";
constexpr auto kTriggerVolumePostSimulation =
    patterns::signature<patterns::signature_length(kTriggerVolumePostSimulationText)>(
        kTriggerVolumePostSimulationText);
constexpr std::size_t kObjectHandleOffset = 44;
constexpr std::size_t kSelectorOffset = 152;
constexpr std::size_t kConditionValueOffset = 156;
constexpr std::size_t kEnabledOffset = 157;
constexpr std::size_t kConditionUsesValueOffset = 158;
constexpr std::size_t kConditionPresentOffset = 159;
constexpr std::size_t kSourceHashOffset = 160;
constexpr std::size_t kActiveOffset = 164;
constexpr std::size_t kTriggerVolumeBaseAdjustment = 24;
constexpr std::size_t kTriggerVolumeBodyOffset = 72;
constexpr std::size_t kTriggerVolumeOverlapCountOffset = 48;
constexpr std::size_t kRigidBodyPositionOffset = 0x1C0;
constexpr ULONGLONG kObservationLifetimeMilliseconds = 2000;

using TriggerTick = std::int64_t(__fastcall*)(void*);
using TriggerVolumePostSimulation = std::int64_t(__fastcall*)(void*);

struct LiveObservation final {
    Observation value{};
    ULONGLONG lastSeen{};
};

std::array<hooking::detour::Handle, 2> g_handles{};
std::atomic<TriggerTick> g_eventOriginal{};
std::atomic<TriggerVolumePostSimulation> g_volumeOriginal{};
std::atomic_bool g_installPublishing{};
std::atomic_bool g_installed{};
std::atomic_bool g_stopping{};
std::atomic_uint g_replacementInFlight{};
SRWLOCK g_lock{SRWLOCK_INIT};
std::array<LiveObservation, kObservationCapacity> g_observations{};
std::uint16_t g_observationCount{};
std::uint64_t g_sequence{};
bool g_truncated{};

struct ReplacementScope final {
    ReplacementScope() noexcept {
        g_replacementInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ReplacementScope() {
        g_replacementInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
    ReplacementScope(const ReplacementScope&) = delete;
    ReplacementScope& operator=(const ReplacementScope&) = delete;
};

template <typename T> [[nodiscard]] T read(const void* address) noexcept {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

[[nodiscard]] bool replacement_idle() noexcept {
    return g_replacementInFlight.load(std::memory_order_acquire) == 0;
}

template <typename T> [[nodiscard]] T original(std::atomic<T>& source) noexcept {
    T next = source.load(std::memory_order_acquire);
    while (next == nullptr && g_installPublishing.load(std::memory_order_acquire)) {
        SwitchToThread();
        next = source.load(std::memory_order_acquire);
    }
    return next;
}

[[nodiscard]] bool same_identity(const Observation& left, const Observation& right) noexcept {
    return left.kind == right.kind && left.observationId == right.observationId;
}

[[nodiscard]] std::uint64_t event_identity(const Observation& observation) noexcept {
    return (static_cast<std::uint64_t>(observation.sourceHash) << 32U)
           | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(observation.selector)) << 16U)
           | observation.objectHandle;
}

[[nodiscard]] std::uint64_t position_identity(const std::array<float, 3>& position) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (float lane : position) {
        const std::uint32_t bits = read<std::uint32_t>(&lane);
        for (std::uint32_t shift = 0; shift < 32; shift += 8) {
            hash ^= (bits >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void publish_observation(const Observation& next) noexcept {
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    std::uint16_t retained = 0;
    for (std::size_t index = 0; index < g_observationCount; ++index) {
        if (now - g_observations[index].lastSeen <= kObservationLifetimeMilliseconds) {
            g_observations[retained++] = g_observations[index];
        }
    }
    g_observationCount = retained;
    if (g_observationCount < g_observations.size()) {
        g_truncated = false;
    }
    for (std::size_t index = 0; index < g_observationCount; ++index) {
        if (same_identity(g_observations[index].value, next)) {
            g_observations[index] = LiveObservation{next, now};
            ++g_sequence;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    if (g_observationCount < g_observations.size()) {
        g_observations[g_observationCount++] = LiveObservation{next, now};
    } else {
        g_truncated = true;
    }
    ++g_sequence;
    ReleaseSRWLockExclusive(&g_lock);
}

void publish_event(void* component) noexcept {
    const auto* const bytes = static_cast<const std::byte*>(component);
    Observation next{};
    next.kind = Kind::event;
    next.objectHandle = read<std::uint16_t>(bytes + kObjectHandleOffset);
    next.selector = read<std::int32_t>(bytes + kSelectorOffset);
    next.enabled = read<std::uint8_t>(bytes + kEnabledOffset) != 0;
    next.sourceHash = read<std::uint32_t>(bytes + kSourceHashOffset);
    next.active = read<std::uint8_t>(bytes + kActiveOffset) != 0;
    if (read<std::uint8_t>(bytes + kConditionPresentOffset) != 0) {
        next.active = read<std::uint8_t>(bytes + kConditionUsesValueOffset) == 0
                      || read<std::uint8_t>(bytes + kConditionValueOffset) != 0;
    }
    next.observationId = event_identity(next);
    publish_observation(next);
}

void publish_volume(void* postSimulationListener) noexcept {
    const auto* const listener = static_cast<const std::byte*>(postSimulationListener);
    const auto* const volume = listener - kTriggerVolumeBaseAdjustment;
    const std::byte* const body = read<std::byte*>(volume + kTriggerVolumeBodyOffset);
    if (body == nullptr) {
        return;
    }
    Observation next{};
    next.kind = Kind::volume;
    next.position = read<std::array<float, 3>>(body + kRigidBodyPositionOffset);
    next.positionPresent = std::ranges::all_of(
        next.position, [](float lane) noexcept { return std::isfinite(lane); });
    if (!next.positionPresent) {
        return;
    }
    const std::int32_t overlapCount =
        read<std::int32_t>(volume + kTriggerVolumeOverlapCountOffset);
    next.overlapCount = overlapCount > 0 ? static_cast<std::uint32_t>(overlapCount) : 0;
    next.active = next.overlapCount != 0;
    next.enabled = true;
    next.observationId = position_identity(next.position);
    publish_observation(next);
}

std::int64_t __fastcall observe_trigger(void* component) noexcept {
    ReplacementScope scope{};
    const TriggerTick next = original(g_eventOriginal);
    if (next == nullptr) {
        return 0;
    }
    if (!g_stopping.load(std::memory_order_acquire) && component != nullptr) {
        publish_event(component);
    }
    return next(component);
}

std::int64_t __fastcall observe_trigger_volume(void* listener) noexcept {
    ReplacementScope scope{};
    const TriggerVolumePostSimulation next = original(g_volumeOriginal);
    if (next == nullptr) {
        return 0;
    }
    if (!g_stopping.load(std::memory_order_acquire) && listener != nullptr) {
        publish_volume(listener);
    }
    return next(listener);
}

void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_observations = {};
    g_observationCount = 0;
    g_sequence = 0;
    g_truncated = false;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    g_stopping.store(false, std::memory_order_release);
    g_installPublishing.store(true, std::memory_order_release);
    std::byte* const eventTarget =
        patterns::scan_main_image_unique(kTriggerTick, "viewer_triggers_trigger_event_tick");
    std::byte* const volumeTarget = patterns::scan_main_image_unique(
        kTriggerVolumePostSimulation, "viewer_triggers_hkp_trigger_volume_post_simulation");
    const std::array specs{
        hooking::detour::Spec{eventTarget, reinterpret_cast<void*>(&observe_trigger)},
        hooking::detour::Spec{volumeTarget, reinterpret_cast<void*>(&observe_trigger_volume)},
    };
    if (eventTarget == nullptr || volumeTarget == nullptr
        || !hooking::detour::install(specs, g_handles)) {
        g_installPublishing.store(false, std::memory_order_release);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=viewer_triggers stage=install result=fail");
        return false;
    }
    g_eventOriginal.store(reinterpret_cast<TriggerTick>(g_handles[0].original),
                          std::memory_order_release);
    g_volumeOriginal.store(reinterpret_cast<TriggerVolumePostSimulation>(g_handles[1].original),
                           std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    g_installPublishing.store(false, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=viewer_triggers stage=install result=ok");
    return true;
}

bool uninstall() noexcept {
    g_stopping.store(true, std::memory_order_release);
    if (!replacement_idle()) {
        return false;
    }
    if (g_handles[0].attached || g_handles[1].attached) {
        const std::array protectedEntries{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe_trigger)},
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe_trigger_volume)},
        };
        const hooking::detour::UninstallResult removal = hooking::detour::uninstall(
            g_handles, protectedEntries, &replacement_idle);
        if (removal != hooking::detour::UninstallResult::removed) {
            return false;
        }
    }
    g_handles = {};
    g_eventOriginal.store(nullptr, std::memory_order_release);
    g_volumeOriginal.store(nullptr, std::memory_order_release);
    g_installed.store(false, std::memory_order_release);
    clear();
    return true;
}

bool snapshot(Snapshot& output) noexcept {
    output = {};
    if (!g_installed.load(std::memory_order_acquire)
        || g_stopping.load(std::memory_order_acquire)) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_lock);
    for (std::size_t index = 0; index < g_observationCount; ++index) {
        const LiveObservation& observation = g_observations[index];
        if (now - observation.lastSeen <= kObservationLifetimeMilliseconds) {
            output.triggers[output.triggerCount++] = observation.value;
        }
    }
    output.sequence = g_sequence;
    output.truncated = g_truncated;
    output.present = true;
    ReleaseSRWLockShared(&g_lock);
    return true;
}

} // namespace sunrise::client::viewer::triggers
