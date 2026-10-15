#include "dynamic_object_registry.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <span>

#include "../../memory/current_process_memory.h"
#include "internal.h"

namespace sunrise::client::hooks::world_objects {
namespace {

/** Object handles address a pool with thirteen index bits. */
constexpr std::size_t kDynamicCapacity = kEntityIndexMask + 1;
/** unknown: the handle lives but its capture failed, so no snapshot is complete. */
struct DynamicSlot final {
    DynamicInstance value{};
    bool present{};
    bool unknown{};
};
struct SpawnIdentity final {
    std::uint32_t classTag{};
};
SRWLOCK g_dynamicLock{SRWLOCK_INIT};
std::array<DynamicSlot, kDynamicCapacity> g_dynamicEntries{};
/** Written under g_dynamicLock; read without it only to skip work. */
std::atomic_bool g_dynamicAvailable{};

/** Native allocation can fail or return while shutdown has stopped observations. */
[[nodiscard]] bool
capture(std::uint32_t handle, const void* entry, DynamicInstance& output) noexcept {
    __try {
        if (g_resolvePair == nullptr || entry == nullptr) {
            return false;
        }
        HandlePair pair{};
        g_resolvePair(&pair, handle);
        DatumIdentity datum{};
        SpawnIdentity source{};
        if (pair.handle != handle || pair.generation == kNone || !read_datum_identity(handle, datum)
            || datum.selfHandle != handle || datum.objectListTag != kNone
            || !memory::read_current_process(nullptr,
                                             reinterpret_cast<std::uintptr_t>(entry),
                                             std::as_writable_bytes(std::span(&source, 1)))) {
            return false;
        }
        output.identity = {datum.placementIdentity,
                           datum.objectListTag,
                           datum.entryIndex,
                           handle,
                           pair.generation};
        output.classTag = source.classTag;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

/** Dynamic identities are isolated from authored duplicate and purge policy. */
void observe_dynamic(std::uint32_t handle, const void* entry) noexcept {
    if (handle == kNone || !g_accepting.load(std::memory_order_acquire)
        || !g_dynamicAvailable.load(std::memory_order_relaxed)) {
        return;
    }
    DynamicInstance value{};
    const bool captured = capture(handle, entry, value);
    AcquireSRWLockExclusive(&g_dynamicLock);
    if (g_dynamicAvailable.load(std::memory_order_relaxed)) {
        g_dynamicEntries[handle & kEntityIndexMask] =
            captured ? DynamicSlot{value, true, false}
                     : DynamicSlot{DynamicInstance{.identity = {.handle = handle}}, false, true};
    }
    ReleaseSRWLockExclusive(&g_dynamicLock);
}

/** Clears the slot only while it still holds this handle; a reused index keeps its new entry. */
void erase_dynamic(std::uint32_t handle) noexcept {
    if (!g_dynamicAvailable.load(std::memory_order_relaxed)) {
        return;
    }
    AcquireSRWLockExclusive(&g_dynamicLock);
    auto& slot = g_dynamicEntries[handle & kEntityIndexMask];
    if ((slot.present || slot.unknown) && slot.value.identity.handle == handle) {
        slot = {};
    }
    ReleaseSRWLockExclusive(&g_dynamicLock);
}

void reset_dynamic_registry(bool available) noexcept {
    AcquireSRWLockExclusive(&g_dynamicLock);
    g_dynamicEntries = {};
    g_dynamicAvailable.store(available, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_dynamicLock);
}

/** An omitted candidate or failed capture makes absence non-authoritative. */
SnapshotResult try_dynamic_snapshot(std::span<DynamicInstance> output) noexcept {
    SnapshotResult result{};
    if (!TryAcquireSRWLockShared(&g_dynamicLock)) {
        result.status = SnapshotStatus::busy;
        return result;
    }
    if (g_dynamicAvailable.load(std::memory_order_relaxed)
        && g_accepting.load(std::memory_order_acquire)) {
        result.status = SnapshotStatus::ready;
        for (const auto& entry : g_dynamicEntries) {
            result.overflow += entry.unknown ? 1U : 0U;
            if (!entry.present) {
                continue;
            }
            ++result.total;
            if (result.written < output.size()) {
                output[result.written++] = entry.value;
            }
        }
    }
    ReleaseSRWLockShared(&g_dynamicLock);
    return result;
}

} // namespace sunrise::client::hooks::world_objects
