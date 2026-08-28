#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <limits>
#include <memory>

#include "../allocator.h"
#include "internal.h"

namespace sunrise::core::ui::memory {
namespace {

constexpr std::size_t kAllocationAlignment = alignof(std::max_align_t);
constexpr std::uint64_t kSpillSignature = 0x53554E5249534550ULL; // "SUNRISEP"

/** Metadata immediately preceding every process-heap spill payload. */
struct alignas(std::max_align_t) SpillHeader {
    std::uint64_t signature{};
    std::size_t requestedBytes{};
    std::size_t totalBytes{};
    void* allocationBase{};
    SpillHeader* previous{};
    SpillHeader* next{};
};

static_assert(sizeof(SpillHeader) % kAllocationAlignment == 0);

ImGuiMemAllocFunc g_previousAllocate{};
ImGuiMemFreeFunc g_previousRelease{};
void* g_previousUserData{};
HANDLE g_processHeap{};
bool g_installed{};
SRWLOCK g_allocatorLock{SRWLOCK_INIT};

std::size_t g_arenaMisses{};
std::size_t g_spillOutstandingAllocations{};
std::size_t g_spillOutstandingBytes{};
std::size_t g_spillHighWaterBytes{};
std::size_t g_allocationFailures{};
std::size_t g_lastFailedBytes{};
SpillHeader* g_spillHead{};

[[nodiscard]] bool add_fits(std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (left > (std::numeric_limits<std::size_t>::max)() - right) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool spill_size(std::size_t requestedBytes, std::size_t& totalBytes) noexcept {
    if (requestedBytes == 0 || !add_fits(requestedBytes, sizeof(SpillHeader), totalBytes)) {
        return false;
    }
    return add_fits(totalBytes, kAllocationAlignment - 1, totalBytes);
}

[[nodiscard]] void* process_heap_allocate(std::size_t bytes) noexcept {
    return HeapAlloc(g_processHeap, 0, bytes);
}

void process_heap_release(void* pointer) noexcept {
    (void)HeapFree(g_processHeap, 0, pointer);
}

/** Returns validated metadata for a live spill payload, without probing foreign memory. */
[[nodiscard]] SpillHeader* spill_header(void* pointer) noexcept {
    if (pointer == nullptr) {
        return nullptr;
    }

    SpillHeader* header = g_spillHead;
    while (header != nullptr
           && reinterpret_cast<std::byte*>(header) + sizeof(SpillHeader) != pointer) {
        header = header->next;
    }
    if (header == nullptr) {
        return nullptr;
    }

    const std::uintptr_t payloadAddress = reinterpret_cast<std::uintptr_t>(pointer);
    if ((payloadAddress % kAllocationAlignment) != 0) {
        return nullptr;
    }
    if (header->signature != kSpillSignature || header->requestedBytes == 0
        || header->allocationBase == nullptr) {
        return nullptr;
    }

    std::size_t expectedTotal{};
    if (!spill_size(header->requestedBytes, expectedTotal) || header->totalBytes != expectedTotal) {
        return nullptr;
    }

    const std::uintptr_t headerAddress = reinterpret_cast<std::uintptr_t>(header);
    const std::uintptr_t baseAddress = reinterpret_cast<std::uintptr_t>(header->allocationBase);
    if (baseAddress > headerAddress || headerAddress - baseAddress > kAllocationAlignment - 1
        || payloadAddress < headerAddress + sizeof(SpillHeader)) {
        return nullptr;
    }
    return header;
}

void release_spill(SpillHeader* header) noexcept {
    void* base = header->allocationBase;
    const std::size_t requestedBytes = header->requestedBytes;
    if (header->previous != nullptr) {
        header->previous->next = header->next;
    } else {
        g_spillHead = header->next;
    }
    if (header->next != nullptr) {
        header->next->previous = header->previous;
    }
    process_heap_release(base);
    --g_spillOutstandingAllocations;
    g_spillOutstandingBytes -= requestedBytes;
}

[[nodiscard]] void* allocate_spill(std::size_t requestedBytes) noexcept {
    std::size_t totalBytes{};
    if (!spill_size(requestedBytes, totalBytes)) {
        return nullptr;
    }

    void* base = process_heap_allocate(totalBytes);
    if (base == nullptr) {
        return nullptr;
    }

    void* payload = static_cast<std::byte*>(base) + sizeof(SpillHeader);
    std::size_t available = totalBytes - sizeof(SpillHeader);
    if (std::align(kAllocationAlignment, requestedBytes, payload, available) == nullptr) {
        process_heap_release(base);
        return nullptr;
    }

    auto* alignedPayload = static_cast<std::byte*>(payload);
    auto* header = reinterpret_cast<SpillHeader*>(alignedPayload - sizeof(SpillHeader));
    header->signature = kSpillSignature;
    header->requestedBytes = requestedBytes;
    header->totalBytes = totalBytes;
    header->allocationBase = base;
    header->previous = nullptr;
    header->next = g_spillHead;
    if (g_spillHead != nullptr) {
        g_spillHead->previous = header;
    }
    g_spillHead = header;
    ++g_spillOutstandingAllocations;
    g_spillOutstandingBytes += requestedBytes;
    g_spillHighWaterBytes = (std::max)(g_spillHighWaterBytes, g_spillOutstandingBytes);
    return alignedPayload;
}

/** Dear ImGui allocation callback: fixed arena first, then tracked process-heap spill. */
[[nodiscard]] void* allocate_callback(std::size_t size, void* userData) noexcept {
    (void)userData;
    AcquireSRWLockExclusive(&g_allocatorLock);
    void* result = nullptr;
    if (g_installed) {
        result = fixed::allocate(size);
        if (result == nullptr) {
            ++g_arenaMisses;
            result = allocate_spill(size);
            if (result == nullptr) {
                ++g_allocationFailures;
                g_lastFailedBytes = size;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_allocatorLock);
    return result;
}

/** Dear ImGui release callback: route arena and validated spill payloads separately. */
void release_callback(void* pointer, void* userData) noexcept {
    (void)userData;
    AcquireSRWLockExclusive(&g_allocatorLock);
    if (g_installed && pointer != nullptr) {
        if (fixed::owns(pointer)) {
            fixed::release(pointer);
        } else if (SpillHeader* header = spill_header(pointer); header != nullptr) {
            release_spill(header);
        }
    }
    ReleaseSRWLockExclusive(&g_allocatorLock);
}

void reset_lifecycle_stats() noexcept {
    g_arenaMisses = 0;
    g_spillOutstandingAllocations = 0;
    g_spillOutstandingBytes = 0;
    g_spillHighWaterBytes = 0;
    g_allocationFailures = 0;
    g_lastFailedBytes = 0;
    g_spillHead = nullptr;
}

} // namespace

/** Installs the arena-first allocator before any Dear ImGui context exists. */
bool initialize() noexcept {
    AcquireSRWLockExclusive(&g_allocatorLock);
    if (g_installed) {
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return true;
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return false;
    }

    ImGui::GetAllocatorFunctions(&g_previousAllocate, &g_previousRelease, &g_previousUserData);
    g_processHeap = GetProcessHeap();
    fixed::reset();
    reset_lifecycle_stats();
    g_installed = true;
    ImGui::SetAllocatorFunctions(allocate_callback, release_callback, nullptr);
    ReleaseSRWLockExclusive(&g_allocatorLock);
    return true;
}

/** Restores the earlier allocator only after arena and spill allocations are both released. */
bool shutdown() noexcept {
    AcquireSRWLockExclusive(&g_allocatorLock);
    if (!g_installed) {
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return true;
    }
    if (fixed::snapshot().outstandingAllocations != 0 || g_spillOutstandingAllocations != 0) {
        // Restoring callbacks early would send later releases to the wrong allocator.
        ReleaseSRWLockExclusive(&g_allocatorLock);
        return false;
    }

    ImGui::SetAllocatorFunctions(g_previousAllocate, g_previousRelease, g_previousUserData);
    fixed::clear();
    g_processHeap = nullptr;
    g_previousAllocate = nullptr;
    g_previousRelease = nullptr;
    g_previousUserData = nullptr;
    reset_lifecycle_stats();
    g_installed = false;
    ReleaseSRWLockExclusive(&g_allocatorLock);
    return true;
}

/** @return One copy of the allocator counters, read under the lock. */
Stats snapshot() noexcept {
    AcquireSRWLockShared(&g_allocatorLock);
    const fixed::ArenaStats arena = fixed::snapshot();
    const Stats result{kArenaCapacityBytes,
                       arena.outstandingBytes,
                       g_installed ? arena.largestFreeBytes : 0,
                       g_arenaMisses,
                       g_spillOutstandingAllocations,
                       g_spillOutstandingBytes,
                       g_spillHighWaterBytes,
                       g_allocationFailures,
                       g_lastFailedBytes};
    ReleaseSRWLockShared(&g_allocatorLock);
    return result;
}

} // namespace sunrise::core::ui::memory
