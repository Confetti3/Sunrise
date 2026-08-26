#include "viewer_objects.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../patterns/image_scan.h"
#include "../teleport/runtime.h"

namespace sunrise::client::viewer::objects {
namespace {

constexpr std::string_view kFinishDatumText =
    "48 89 5C 24 ? 57 48 83 EC 20 8B F9 8B D9 81 E7 FF 1F 00 00 0F AF 3D ? ? ? ? "
    "48 03 3D ? ? ? ? E8";
constexpr auto kFinishDatum =
    patterns::signature<patterns::signature_length(kFinishDatumText)>(kFinishDatumText);

constexpr std::string_view kIteratorInitText =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC 20 "
    "48 8B 05 ? ? ? ? 48 8D 2D ? ? ? ?";
constexpr auto kIteratorInit =
    patterns::signature<patterns::signature_length(kIteratorInitText)>(kIteratorInitText);

constexpr std::string_view kIteratorNextText =
    "40 53 48 83 EC 20 48 8B D9 48 83 C1 08 E8 ? ? ? ? 48 8B CB 48 83 C4 20 5B E9 ? ? ? ?";
constexpr auto kIteratorNext =
    patterns::signature<patterns::signature_length(kIteratorNextText)>(kIteratorNextText);

constexpr std::size_t kIteratorBytes = 0x38;
constexpr std::size_t kIteratorIndex = 0x08;
constexpr std::size_t kIteratorLimit = 0x18;
constexpr std::size_t kIteratorDescriptor = 0x20;
constexpr std::uint16_t kIteratorEnd = 0xFFFF;
constexpr std::uint32_t kObjectIndexMask = 0x1FFF;
constexpr std::size_t kObjectType = 0x08;
constexpr std::uint32_t kExpectedObjectStride = 224;
constexpr std::uint32_t kMaximumObjectSlots = 4096;
constexpr ULONGLONG kPollIntervalMilliseconds = 200;
constexpr std::size_t kPositionCapacity = 512;
constexpr std::size_t kComponentObjectHandle = 44;

using IteratorInit = void*(__fastcall*)(void*, std::int32_t);
using IteratorNext = void(__fastcall*)(void*);

struct PositionObservation final {
    std::uint16_t handle{};
    std::array<float, 3> position{};
};

std::atomic_bool g_installed{false};
std::atomic<ULONGLONG> g_lastPollTick{};
IteratorInit g_iteratorInit{};
IteratorNext g_iteratorNext{};
std::byte** g_objectBase{};
std::uint32_t* g_objectStride{};
SRWLOCK g_lock{SRWLOCK_INIT};
Snapshot g_snapshot{};
std::array<PositionObservation, kPositionCapacity> g_positions{};
std::uint16_t g_positionCount{};
std::uint64_t g_sequence{};

template <typename T> [[nodiscard]] T read(const void* address) noexcept {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

[[nodiscard]] bool finite(const std::array<float, 3>& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] std::uint32_t current_handle(const std::array<std::byte, kIteratorBytes>& iterator,
                                           bool& present) noexcept {
    present = false;
    const std::int32_t index = read<std::int32_t>(iterator.data() + kIteratorIndex);
    const std::int32_t limit = read<std::int32_t>(iterator.data() + kIteratorLimit);
    const std::uint16_t slot = index < limit ? static_cast<std::uint16_t>(index) : kIteratorEnd;
    if (slot == kIteratorEnd) {
        return 0;
    }
    std::byte* const descriptor = read<std::byte*>(iterator.data() + kIteratorDescriptor);
    if (descriptor == nullptr) {
        return 0;
    }
    const std::uint32_t generationStride = read<std::uint32_t>(descriptor + 0x20);
    const std::uint32_t generationOffset = read<std::uint32_t>(descriptor + 0x1C);
    const std::uint32_t generationMask = read<std::uint32_t>(descriptor + 0x24);
    const std::uint32_t encoding = read<std::uint32_t>(descriptor + 0x34);
    std::byte* const generationBase = read<std::byte*>(descriptor + 0x08);
    if (generationBase == nullptr || generationStride == 0) {
        return 0;
    }
    const std::uint32_t generation =
        read<std::uint32_t>(generationBase + generationOffset
                            + static_cast<std::size_t>(slot) * generationStride)
        & generationMask;
    std::uint32_t high = 0;
    if ((encoding & 0x40000000U) != 0) {
        high = ((generation & 7U) | 0xFFFFFFF0U) << 14U;
        high |= encoding & 0x3FFFU;
    } else {
        high = ((generation & 0xFFU) << 10U) | (encoding & 0x3FFU);
    }
    present = true;
    return (high << 13U) | slot;
}

void merge_position(Observation& object,
                    const std::array<PositionObservation, kPositionCapacity>& positions,
                    std::uint16_t count) noexcept {
    const std::uint16_t key = static_cast<std::uint16_t>(object.handle);
    for (std::size_t index = 0; index < count; ++index) {
        if (positions[index].handle == key) {
            object.position = positions[index].position;
            object.positionPresent = true;
            return;
        }
    }
}

void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_snapshot = {};
    g_positions = {};
    g_positionCount = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const finish =
        patterns::scan_main_image_unique(kFinishDatum, "viewer_objects_finish_datum");
    std::byte* const init =
        patterns::scan_main_image_unique(kIteratorInit, "viewer_objects_iterator_init");
    std::byte* const next =
        patterns::scan_main_image_unique(kIteratorNext, "viewer_objects_iterator_next");
    if (finish == nullptr || init == nullptr || next == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=viewer_objects stage=install result=fail reason=signature");
        return false;
    }
    g_objectStride =
        reinterpret_cast<std::uint32_t*>(patterns::resolve_relative(finish + 23, finish + 27));
    g_objectBase =
        reinterpret_cast<std::byte**>(patterns::resolve_relative(finish + 30, finish + 34));
    g_iteratorInit = reinterpret_cast<IteratorInit>(init);
    g_iteratorNext = reinterpret_cast<IteratorNext>(next);
    g_lastPollTick.store(0, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=viewer_objects stage=install result=ok mode=official_iterator");
    return true;
}

void uninstall() noexcept {
    g_installed.store(false, std::memory_order_release);
    g_iteratorInit = nullptr;
    g_iteratorNext = nullptr;
    g_objectBase = nullptr;
    g_objectStride = nullptr;
    g_lastPollTick.store(0, std::memory_order_release);
    clear();
}

void reset_activity() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_positions = {};
    g_positionCount = 0;
    for (std::size_t index = 0; index < g_snapshot.objectCount; ++index) {
        g_snapshot.objects[index].position = {};
        g_snapshot.objects[index].positionPresent = false;
    }
    ++g_sequence;
    g_snapshot.sequence = g_sequence;
    ReleaseSRWLockExclusive(&g_lock);
}

void observe_physics_component(void* component) noexcept {
    if (!g_installed.load(std::memory_order_acquire) || component == nullptr) {
        return;
    }
    std::array<float, 3> position{};
    if (!hooks::teleport::read_position(component, position) || !finite(position)) {
        return;
    }
    const auto* const bytes = static_cast<const std::byte*>(component);
    const std::uint16_t handle = read<std::uint16_t>(bytes + kComponentObjectHandle);
    AcquireSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < g_positionCount; ++index) {
        if (g_positions[index].handle == handle) {
            g_positions[index].position = position;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    if (g_positionCount < g_positions.size()) {
        g_positions[g_positionCount++] = PositionObservation{handle, position};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void poll() noexcept {
    if (!g_installed.load(std::memory_order_acquire) || g_iteratorInit == nullptr
        || g_iteratorNext == nullptr || g_objectBase == nullptr || g_objectStride == nullptr) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = g_lastPollTick.load(std::memory_order_relaxed);
    if (now - previous < kPollIntervalMilliseconds
        || !g_lastPollTick.compare_exchange_strong(previous, now, std::memory_order_acq_rel)) {
        return;
    }
    std::byte* const base = *g_objectBase;
    const std::uint32_t stride = *g_objectStride;
    if (base == nullptr || stride != kExpectedObjectStride) {
        clear();
        return;
    }

    std::array<PositionObservation, kPositionCapacity> positions{};
    std::uint16_t positionCount = 0;
    AcquireSRWLockShared(&g_lock);
    positions = g_positions;
    positionCount = g_positionCount;
    ReleaseSRWLockShared(&g_lock);

    Snapshot next{};
    alignas(16) std::array<std::byte, kIteratorBytes> iterator{};
    (void)g_iteratorInit(iterator.data(), 0);
    for (std::uint32_t visited = 0;; ++visited) {
        bool occupied = false;
        const std::uint32_t handle = current_handle(iterator, occupied);
        if (!occupied) {
            break;
        }
        if (visited >= kMaximumObjectSlots) {
            next.truncated = true;
            break;
        }
        const std::uint32_t slot = handle & kObjectIndexMask;
        const std::uint8_t type =
            read<std::uint8_t>(base + static_cast<std::size_t>(slot) * stride + kObjectType);
        // Effect emitters are transient and are not stable Inspector identities. Exclude them
        // before applying the bounded snapshot capacity so their churn cannot change retained
        // membership or truncation state.
        if (type == 0x11) {
            g_iteratorNext(iterator.data());
            continue;
        }
        ++next.declaredCount;
        if (next.objectCount < next.objects.size()) {
            Observation& output = next.objects[next.objectCount++];
            output.handle = handle;
            output.type = type;
            merge_position(output, positions, positionCount);
        } else {
            next.truncated = true;
        }
        g_iteratorNext(iterator.data());
    }
    next.present = true;
    next.sequence = ++g_sequence;
    AcquireSRWLockExclusive(&g_lock);
    g_snapshot = next;
    ReleaseSRWLockExclusive(&g_lock);
}

bool snapshot(Snapshot& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    output = g_snapshot;
    ReleaseSRWLockShared(&g_lock);
    return output.present;
}

bool installed() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

} // namespace sunrise::client::viewer::objects
