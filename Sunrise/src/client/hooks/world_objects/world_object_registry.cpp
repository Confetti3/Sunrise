#include "world_object_registry.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../executable/image.h"
#include "../../hooking/detour.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/registry.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::world_objects {
namespace {

using patterns::signature;
using patterns::signature_length;

constexpr std::uint32_t kNone = 0xFFFFFFFFU;

// Identity-bearing placements per registry clear that get an observed line. This is the positive
// control for the duplicate count, so it matches the registry's own capacity. A smaller budget is
// spent before the object lists load, and a missing duplicate then says nothing.
/** Dynamic handles tracked for teardown. One Tower bubble builds at most 73 of them. */
constexpr std::size_t kDynamicHandleCapacity = 256;
/** Stack frames reported above one off-ledger allocation. */
constexpr std::size_t kFrameCount = 4;
constexpr std::size_t kIdentityReportBudget = 16384;

constexpr std::size_t kRegistryCapacity = 16384;
constexpr std::size_t kRegistryMask = kRegistryCapacity - 1;
static_assert((kRegistryCapacity & kRegistryMask) == 0);

constexpr std::string_view kInstantiateSignatureText =
    "40 55 53 56 41 56 41 57 48 8D 6C 24 ? 48 81 EC 60 01 00 00";
constexpr auto kInstantiateSignature =
    signature<signature_length(kInstantiateSignatureText)>(kInstantiateSignatureText);

constexpr std::string_view kDestroySignatureText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 50 8B D9 8B F9";
constexpr auto kDestroySignature =
    signature<signature_length(kDestroySignatureText)>(kDestroySignatureText);

// The datum allocator every creation path shares. The instantiate hook above is one of its
// three callers, so a build reported here and not there was made by one of the other two.
constexpr std::string_view kAllocateSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 56 48 81 EC 40 01 00 00 48 8B 05 ? ? ? ? 48 33 "
    "C4 48 89 84 24 ? ? ? ? C7 01 FF FF FF FF";
constexpr auto kAllocateSignature =
    signature<signature_length(kAllocateSignatureText)>(kAllocateSignatureText);

// The logical destroy. It is the only path that releases an object's simulation entity, so a
// teardown that frees the object without passing here leaves the entity to rebuild it.
constexpr std::string_view kLogicalDestroySignatureText =
    "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40 48 8B 3D ? ? ? ? 8B D9";
constexpr auto kLogicalDestroySignature =
    signature<signature_length(kLogicalDestroySignatureText)>(kLogicalDestroySignatureText);

constexpr std::string_view kResolvePairSignatureText =
    "4C 8B D1 83 FA FF 74 ? 4C 8B 0D ? ? ? ? 44 8B C2 41 C1 F8 1F 8B C2 C1 E8 0D 41 81 E0 "
    "00 3C 00 00 0F B7 C8 41 81 C8 FF 03 00 00 49 8B 01 44 23 C1 45 0F AF 41 ? 0F B7 CA "
    "81 E1 FF 1F 00 00 4D 8B 44 00";
constexpr auto kResolvePairSignature =
    signature<signature_length(kResolvePairSignatureText)>(kResolvePairSignatureText);

constexpr std::string_view kValidatePairSignatureText = "48 83 EC 08 44 8B 51";
constexpr auto kValidatePairSignature =
    signature<signature_length(kValidatePairSignatureText)>(kValidatePairSignatureText);

// The longer suffix fixes the two RIP-relative operands used below at stable byte offsets.
constexpr std::string_view kDatumLayoutSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 55 41 56 41 57 48 83 EC 30 44 8B F1 8B F9 41 81 "
    "E6 FF 1F 00 00 48 8B CA 44 0F AF 35 ? ? ? ? 41 8B E9 41 8B D8 4C 8B FA 4C 03 35 ? ? "
    "? ?";
constexpr auto kDatumLayoutSignature =
    signature<signature_length(kDatumLayoutSignatureText)>(kDatumLayoutSignatureText);

using Instantiate = std::uint32_t*(__fastcall*)(std::uint32_t*,
                                                const void*,
                                                std::int32_t,
                                                std::int32_t) noexcept;
using Destroy = std::uintptr_t(__fastcall*)(std::uint32_t) noexcept;
using Allocate = Instantiate;
using LogicalDestroy = std::uintptr_t(__fastcall*)(std::uint32_t) noexcept;

struct HandlePair final {
    std::uint32_t generation{kNone};
    std::uint32_t handle{kNone};
};

using ResolvePair = std::uintptr_t(__fastcall*)(HandlePair*, std::uint32_t) noexcept;
using ValidatePair = std::int32_t*(__fastcall*)(const HandlePair*, std::int32_t*) noexcept;

enum class EntryState : std::uint8_t {
    empty,
    live,
    tombstone,
};

struct RegistryEntry final {
    Instance instance{};
    EntryState state{EntryState::empty};
};

/** One fixed placement identity count, separate from the handle-keyed lifetime table. */
struct IdentityEntry final {
    std::uint64_t key{};
    std::uint32_t count{};
    EntryState state{EntryState::empty};
};

struct DatumIdentity final {
    std::array<std::byte, 12> prefix{};
    std::uint32_t selfHandle{};
    std::array<std::byte, 120> middle{};
    std::uint32_t objectListTag{};
    std::uint32_t entryIndex{};
    std::uint64_t placementIdentity{};
};

static_assert(offsetof(DatumIdentity, selfHandle) == 0x0C);
static_assert(offsetof(DatumIdentity, objectListTag) == 0x88);
static_assert(offsetof(DatumIdentity, entryIndex) == 0x8C);
static_assert(offsetof(DatumIdentity, placementIdentity) == 0x90);

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<RegistryEntry, kRegistryCapacity> g_entries{};
std::array<IdentityEntry, kRegistryCapacity> g_identities{};
std::size_t g_liveCount{};
std::uint64_t g_overflowCount{};
std::size_t g_identityReportBudget{kIdentityReportBudget};
std::size_t g_dynamicReportBudget{kIdentityReportBudget};
std::uint64_t g_dynamicCount{};
/**
 * Handles the dynamic path built, so the destroy detour can say which of them the client tears
 * down. A dynamic build names no placed entry, so the identity registry never holds one, and
 * whether a bubble crossing removes it is not known.
 */
std::array<std::uint32_t, kDynamicHandleCapacity> g_dynamicHandles{};
std::array<std::uint64_t, kDynamicHandleCapacity> g_dynamicOrdinals{};
std::atomic_uint32_t g_activeCalls{};
std::atomic_bool g_accepting{};
std::array<hooking::detour::Handle, 4> g_handles{};
std::atomic<Instantiate> g_instantiateOriginal{nullptr};
std::atomic<Destroy> g_destroyOriginal{nullptr};
std::atomic<Allocate> g_allocateOriginal{nullptr};
std::atomic<LogicalDestroy> g_logicalDestroyOriginal{nullptr};
std::size_t g_logicalDestroyReportBudget{kIdentityReportBudget};

/** Set while this thread is inside the instantiate detour, whose allocation is already reported. */
thread_local bool t_inInstantiate{};
std::size_t g_allocateReportBudget{kIdentityReportBudget};
ResolvePair g_resolvePair{};
ValidatePair g_validatePair{};
const std::uintptr_t* g_datumBaseStorage{};
// Main module base, so a caller address is reported as an RVA and stays comparable
// against the IDB across runs.
std::uintptr_t g_moduleBase{};
const std::uint32_t* g_datumStrideStorage{};

/** Counts one in-flight detour call, so teardown can wait for the hooks to drain. */
class ActiveCall final {
public:
    ActiveCall() noexcept {
        g_activeCalls.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ActiveCall() {
        g_activeCalls.fetch_sub(1, std::memory_order_acq_rel);
    }

    ActiveCall(const ActiveCall&) = delete;
    ActiveCall& operator=(const ActiveCall&) = delete;
};

/** @return A stable open-address bucket for a native handle. */
[[nodiscard]] constexpr std::size_t bucket(std::uint32_t handle) noexcept {
    return (static_cast<std::size_t>(handle) * 2654435761U) & kRegistryMask;
}

/** Clears every retained identity. Caller owns g_lock exclusively. */
void clear_registry() noexcept {
    g_entries = {};
    g_identities = {};
    g_liveCount = 0;
    g_identityReportBudget = kIdentityReportBudget;
    g_dynamicReportBudget = kIdentityReportBudget;
    g_allocateReportBudget = kIdentityReportBudget;
    g_logicalDestroyReportBudget = kIdentityReportBudget;
    g_dynamicCount = 0;
}

/** @return True when the datum retained a real placed-entry identity. */
[[nodiscard]] constexpr bool has_placement_identity(const Instance& instance) noexcept {
    return instance.placementIdentity != 0
           && instance.placementIdentity != kAbsentPlacementIdentity;
}

/**
 * Combines one placement into a nonzero registry key.
 * A real placed-entry identity wins: a mirrored map variant repeats it under a different
 * object-list tag, which the tuple alone would count twice. No identity means the tuple key.
 */
[[nodiscard]] constexpr std::uint64_t identity_key(const Instance& instance) noexcept {
    if (has_placement_identity(instance)) {
        return instance.placementIdentity;
    }
    return (static_cast<std::uint64_t>(instance.objectListTag) << 32U) | instance.entryIndex;
}

/** Adjusts one placement count and reports the first simultaneous duplicate. */
void adjust_identity(const Instance& instance, bool increment) noexcept {
    const std::uint64_t key = identity_key(instance);
    const std::size_t first =
        static_cast<std::size_t>((key ^ (key >> 32U)) * 2654435761U) & kRegistryMask;
    std::size_t tombstone = kRegistryCapacity;
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        const std::size_t index = (first + probe) & kRegistryMask;
        IdentityEntry& entry = g_identities[index];
        if (entry.state == EntryState::live && entry.key == key) {
            if (increment) {
                ++entry.count;
                if (entry.count == 2) {
                    std::array<char, 192> line{};
                    const int written = std::snprintf(
                        line.data(),
                        line.size(),
                        "ev=world_object stage=placement result=duplicate list=0x%08X "
                        "entry=%u identity=0x%016llX count=%u",
                        instance.objectListTag,
                        instance.entryIndex,
                        static_cast<unsigned long long>(instance.placementIdentity),
                        entry.count);
                    if (written > 0) {
                        core::log::write(core::log::Channel::client,
                                         core::log::Level::debug,
                                         {line.data(), static_cast<std::size_t>(written)});
                    }
                }
            } else if (entry.count > 1) {
                --entry.count;
            } else {
                entry.state = EntryState::tombstone;
                entry.count = 0;
            }
            return;
        }
        if (entry.state == EntryState::tombstone && tombstone == kRegistryCapacity) {
            tombstone = index;
        }
        if (entry.state != EntryState::empty) {
            continue;
        }
        if (!increment) {
            return;
        }
        IdentityEntry& destination =
            g_identities[tombstone == kRegistryCapacity ? index : tombstone];
        destination = {key, 1, EntryState::live};
        return;
    }
    if (increment && tombstone < kRegistryCapacity) {
        g_identities[tombstone] = {key, 1, EntryState::live};
    }
}

/** Inserts or replaces one handle without allocating. Caller owns g_lock exclusively. */
void retain(const Instance& instance) noexcept {
    std::size_t firstTombstone = kRegistryCapacity;
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        const std::size_t index = (bucket(instance.handle) + probe) & kRegistryMask;
        RegistryEntry& entry = g_entries[index];
        if (entry.state == EntryState::live && entry.instance.handle == instance.handle) {
            if (identity_key(entry.instance) != identity_key(instance)) {
                adjust_identity(entry.instance, false);
                adjust_identity(instance, true);
            }
            entry.instance = instance;
            return;
        }
        if (entry.state == EntryState::tombstone && firstTombstone == kRegistryCapacity) {
            firstTombstone = index;
        }
        if (entry.state != EntryState::empty) {
            continue;
        }
        RegistryEntry& destination =
            g_entries[firstTombstone == kRegistryCapacity ? index : firstTombstone];
        destination.instance = instance;
        destination.state = EntryState::live;
        adjust_identity(instance, true);
        ++g_liveCount;
        return;
    }
    if (firstTombstone != kRegistryCapacity) {
        RegistryEntry& destination = g_entries[firstTombstone];
        destination.instance = instance;
        destination.state = EntryState::live;
        adjust_identity(instance, true);
        ++g_liveCount;
        return;
    }
    ++g_overflowCount;
}

/**
 * Returns a tombstone run to empty when the slot after it is already empty.
 * A probe stops only on empty, so a run that never becomes empty makes every later miss walk the
 * whole table. Nothing live can sit past an empty slot, so the run behind one is free to reclaim.
 * @param index Slot just tombstoned. Caller owns g_lock exclusively.
 */
void reclaim_tombstones(std::size_t index) noexcept {
    if (g_entries[(index + 1) & kRegistryMask].state != EntryState::empty) {
        return;
    }
    std::size_t slot = index;
    while (g_entries[slot].state == EntryState::tombstone) {
        g_entries[slot] = {};
        slot = (slot - 1) & kRegistryMask;
    }
}

/** Erases one handle before native teardown can recycle its generation. */
void erase(std::uint32_t handle) noexcept {
    for (std::size_t probe = 0; probe < kRegistryCapacity; ++probe) {
        const std::size_t index = (bucket(handle) + probe) & kRegistryMask;
        RegistryEntry& entry = g_entries[index];
        if (entry.state == EntryState::empty) {
            return;
        }
        if (entry.state == EntryState::live && entry.instance.handle == handle) {
            adjust_identity(entry.instance, false);
            entry.state = EntryState::tombstone;
            --g_liveCount;
            if (g_liveCount == 0) {
                clear_registry();
                return;
            }
            reclaim_tombstones(index);
            return;
        }
    }
}

/** Reads one scalar from the process without trusting a stale game-owned pointer. */
template <typename Value>
[[nodiscard]] bool read_value(const Value* source, Value& output) noexcept {
    return memory::read_current_process(
        nullptr,
        reinterpret_cast<std::uintptr_t>(source),
        std::span(reinterpret_cast<std::byte*>(&output), sizeof output));
}

/** Datum bytes through the object-list tuple, and through the retained placement identity. */
constexpr std::uint32_t kDatumTupleBytes = offsetof(DatumIdentity, placementIdentity);
constexpr std::uint32_t kDatumIdentityBytes = sizeof(DatumIdentity);

/**
 * Reads one object datum's identity block.
 * A stride that stops before the placement identity still yields the tuple, and leaves the
 * identity zero, so a layout surprise degrades to tuple-keyed counting instead of silence.
 */
[[nodiscard]] bool read_datum_identity(std::uint32_t handle, DatumIdentity& output) noexcept {
    if (g_datumBaseStorage == nullptr || g_datumStrideStorage == nullptr) {
        return false;
    }
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!read_value(g_datumBaseStorage, base) || !read_value(g_datumStrideStorage, stride)
        || base == 0 || stride < kDatumTupleBytes) {
        return false;
    }
    const std::size_t wanted =
        stride >= kDatumIdentityBytes ? kDatumIdentityBytes : kDatumTupleBytes;
    output = {};
    return memory::read_current_process(
        nullptr,
        base + static_cast<std::uintptr_t>(stride) * (handle & 0x1FFFU),
        std::span(reinterpret_cast<std::byte*>(&output), wanted));
}

/** Checks the native generation pair and the three datum identity fields. */
[[nodiscard]] bool validate(const Instance& instance) noexcept {
    if (g_validatePair == nullptr) {
        return false;
    }
    const HandlePair pair{instance.generation, instance.handle};
    std::int32_t resolved = -1;
    __try {
        g_validatePair(&pair, &resolved);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (static_cast<std::uint32_t>(resolved) != instance.handle) {
        return false;
    }

    DatumIdentity identity{};
    if (!read_datum_identity(instance.handle, identity)) {
        return false;
    }
    return identity.selfHandle == instance.handle
           && identity.objectListTag == instance.objectListTag
           && identity.entryIndex == instance.entryIndex;
}

/**
 * Reads the placed entry's authored world position, held at entry `+0x20`.
 * Two entries at one position under different object lists are the same authored prop placed
 * twice, which an identity-keyed count cannot see because each source carries its own `+0x70`.
 */
[[nodiscard]] bool read_entry_position(const void* entry, std::array<float, 3>& output) noexcept {
    if (entry == nullptr) {
        return false;
    }
    return memory::read_current_process(
        nullptr,
        reinterpret_cast<std::uintptr_t>(entry) + 0x20U,
        std::span(reinterpret_cast<std::byte*>(output.data()), sizeof(float) * output.size()));
}

/**
 * Reports one object built by a caller that named no placed entry.
 * Those callers pass {-1,-1}, so the registry cannot retain them and nothing else here sees them.
 * A second copy of authored content that the placed path builds once must come from here.
 */
void report_dynamic(std::uint32_t handle, const void* entry, std::uintptr_t caller) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    ++g_dynamicCount;
    const std::uint64_t ordinal = g_dynamicCount;
    const bool report = g_dynamicReportBudget > 0;
    if (report) {
        --g_dynamicReportBudget;
    }
    const std::size_t track = static_cast<std::size_t>(ordinal - 1) % kDynamicHandleCapacity;
    g_dynamicHandles[track] = handle;
    g_dynamicOrdinals[track] = ordinal;
    ReleaseSRWLockExclusive(&g_lock);
    if (!report) {
        return;
    }
    std::array<float, 3> position{};
    const bool positionRead = read_entry_position(entry, position);
    std::array<char, 224> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=world_object stage=placement result=dynamic "
        "handle=0x%08X ordinal=%llu pos=%.3f,%.3f,%.3f "
        "caller=+0x%llX",
        handle,
        static_cast<unsigned long long>(ordinal),
        positionRead ? static_cast<double>(position[0]) : 0.0,
        positionRead ? static_cast<double>(position[1]) : 0.0,
        positionRead ? static_cast<double>(position[2]) : 0.0,
        static_cast<unsigned long long>(caller >= g_moduleBase ? caller - g_moduleBase : caller));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Captures only placed-content calls; dynamic callers pass {-1,-1}. */
void observe_instance(std::uint32_t handle,
                      const void* entry,
                      std::int32_t objectListTag,
                      std::int32_t entryIndex) noexcept {
    if (handle == kNone || objectListTag == -1 || entryIndex == -1 || g_resolvePair == nullptr
        || !g_accepting.load(std::memory_order_acquire)) {
        return;
    }
    HandlePair pair{};
    __try {
        g_resolvePair(&pair, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (pair.handle != handle || pair.generation == kNone) {
        return;
    }
    // Native init has already copied placed entry +0x70 into the datum at +0x90, so the identity
    // that survives a mirrored object list is readable here.
    DatumIdentity datum{};
    const bool datumRead = read_datum_identity(handle, datum) && datum.selfHandle == handle
                           && datum.objectListTag == static_cast<std::uint32_t>(objectListTag)
                           && datum.entryIndex == static_cast<std::uint32_t>(entryIndex);
    const Instance instance{datumRead ? datum.placementIdentity : 0,
                            static_cast<std::uint32_t>(objectListTag),
                            static_cast<std::uint32_t>(entryIndex),
                            handle,
                            pair.generation};
    AcquireSRWLockExclusive(&g_lock);
    retain(instance);
    const bool report = has_placement_identity(instance) && g_identityReportBudget > 0;
    if (report) {
        --g_identityReportBudget;
    }
    const std::size_t live = g_liveCount;
    ReleaseSRWLockExclusive(&g_lock);
    if (!report) {
        return;
    }
    std::array<float, 3> position{};
    const bool positionRead = read_entry_position(entry, position);
    std::array<char, 256> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=world_object stage=placement result=observed "
                                      "list=0x%08X entry=%u identity=0x%016llX handle=0x%08X "
                                      "pos=%.3f,%.3f,%.3f live=%zu",
                                      instance.objectListTag,
                                      instance.entryIndex,
                                      static_cast<unsigned long long>(instance.placementIdentity),
                                      instance.handle,
                                      positionRead ? static_cast<double>(position[0]) : 0.0,
                                      positionRead ? static_cast<double>(position[1]) : 0.0,
                                      positionRead ? static_cast<double>(position[2]) : 0.0,
                                      live);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Calls native construction first, then retains the successfully initialized identity. */
__declspec(noinline) std::uint32_t* __fastcall instantiate(std::uint32_t* output,
                                                           const void* entry,
                                                           std::int32_t objectListTag,
                                                           std::int32_t entryIndex) noexcept {
    ActiveCall active;
    const Instantiate original = g_instantiateOriginal.load(std::memory_order_acquire);
    t_inInstantiate = true;
    std::uint32_t* const result =
        original != nullptr ? original(output, entry, objectListTag, entryIndex) : output;
    t_inInstantiate = false;
    if (result != nullptr) {
        if (objectListTag == -1 || entryIndex == -1) {
            report_dynamic(*result, entry, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        } else {
            observe_instance(*result, entry, objectListTag, entryIndex);
        }
    }
    return result;
}

/**
 * Reports one datum allocated by a path other than the instantiate hook above.
 * @param caller Return address, reported as an RVA so it names the creating function.
 */
/** @return The datum flag word at `+4`, or zero when it cannot be read. */
[[nodiscard]] std::uint32_t datum_flags(std::uint32_t handle) noexcept {
    if (g_datumBaseStorage == nullptr || g_datumStrideStorage == nullptr) {
        return 0;
    }
    const std::uintptr_t datum =
        *g_datumBaseStorage
        + static_cast<std::uintptr_t>(*g_datumStrideStorage) * (handle & 0x1FFFU);
    std::uint32_t flags = 0;
    if (!memory::read_current_process(
            nullptr, datum + 4U, std::span(reinterpret_cast<std::byte*>(&flags), sizeof flags))) {
        return 0;
    }
    return flags;
}

void report_allocation(std::uint32_t handle, std::uintptr_t caller) noexcept {
    // The allocation site alone does not say which subsystem asked. Unwind names the frames.
    std::array<void*, kFrameCount> frames{};
    const USHORT captured =
        RtlCaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    AcquireSRWLockExclusive(&g_lock);
    const bool report = g_allocateReportBudget > 0;
    if (report) {
        --g_allocateReportBudget;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!report) {
        return;
    }
    std::array<char, 192> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=world_object stage=placement result=alloc handle=0x%08X flags=0x%08X caller=+0x%llX",
        handle,
        datum_flags(handle),
        static_cast<unsigned long long>(caller >= g_moduleBase ? caller - g_moduleBase : caller));
    for (USHORT index = 0; written > 0 && index < captured; ++index) {
        const auto frame = reinterpret_cast<std::uintptr_t>(frames[index]);
        const int appended = std::snprintf(
            line.data() + written,
            line.size() - static_cast<std::size_t>(written),
            " f%u=+0x%llX",
            static_cast<unsigned>(index),
            static_cast<unsigned long long>(frame >= g_moduleBase ? frame - g_moduleBase : frame));
        if (appended <= 0) {
            break;
        }
        written += appended;
    }
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Calls native allocation first, then reports it when the instantiate hook did not ask. */
__declspec(noinline) std::uint32_t* __fastcall allocate(std::uint32_t* output,
                                                        const void* entry,
                                                        std::int32_t objectListTag,
                                                        std::int32_t entryIndex) noexcept {
    ActiveCall active;
    const Allocate original = g_allocateOriginal.load(std::memory_order_acquire);
    std::uint32_t* const result =
        original != nullptr ? original(output, entry, objectListTag, entryIndex) : output;
    if (!t_inInstantiate && result != nullptr && *result != kNone
        && g_accepting.load(std::memory_order_acquire)) {
        report_allocation(*result, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    }
    return result;
}

/** Reports one logical destroy, the only teardown that releases the object's entity. */
__declspec(noinline) std::uintptr_t __fastcall logical_destroy(std::uint32_t handle) noexcept {
    ActiveCall active;
    const LogicalDestroy original = g_logicalDestroyOriginal.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_lock);
    const bool report = g_logicalDestroyReportBudget > 0;
    if (report) {
        --g_logicalDestroyReportBudget;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (report && g_accepting.load(std::memory_order_acquire)) {
        std::array<char, 128> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=world_object stage=placement result=released handle=0x%08X "
                          "caller=+0x%llX",
                          handle,
                          static_cast<unsigned long long>(
                              reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return original != nullptr ? original(handle) : 0;
}

/** Reports the teardown of one object the dynamic path built, and forgets it. */
void report_dynamic_destroy(std::uint32_t handle) noexcept {
    std::uint64_t ordinal = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < kDynamicHandleCapacity; ++index) {
        if (g_dynamicHandles[index] == handle && g_dynamicOrdinals[index] != 0) {
            ordinal = g_dynamicOrdinals[index];
            g_dynamicHandles[index] = 0;
            g_dynamicOrdinals[index] = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (ordinal == 0) {
        return;
    }
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=world_object stage=placement result=dynamic_destroyed "
                                      "handle=0x%08X ordinal=%llu",
                                      handle,
                                      static_cast<unsigned long long>(ordinal));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Removes the identity while its datum and generation still belong to this handle. */
__declspec(noinline) std::uintptr_t __fastcall destroy(std::uint32_t handle) noexcept {
    ActiveCall active;
    if (g_accepting.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&g_lock);
        erase(handle);
        ReleaseSRWLockExclusive(&g_lock);
        report_dynamic_destroy(handle);
    }
    const Destroy original = g_destroyOriginal.load(std::memory_order_acquire);
    return original != nullptr ? original(handle) : 0;
}

/** @return True when no replacement still owns a trampoline call. */
[[nodiscard]] bool calls_idle() noexcept {
    return g_activeCalls.load(std::memory_order_acquire) == 0;
}

struct Targets final {
    std::byte* instantiate{};
    std::byte* destroy{};
    std::byte* allocate{};
    std::byte* logicalDestroy{};
    std::byte* resolvePair{};
    std::byte* validatePair{};
    std::byte* datumLayout{};
};

/** @return The main module's base, or zero when it cannot be read. */
[[nodiscard]] std::uintptr_t main_module_base() noexcept {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

/** Resolves all five optional targets in one image pass. */
[[nodiscard]] bool resolve_targets(Targets& output) noexcept {
    output = {};
    executable::ExecutableImage main{};
    if (!executable::inspect_main_module(main)) {
        return false;
    }
    std::array<patterns::ImageRange, executable::kPeSectionLimit> ranges{};
    for (std::size_t index = 0; index < main.count; ++index) {
        ranges[index] = patterns::ImageRange{main.sections[index]};
    }
    const std::array definitions{
        patterns::Pattern{"placed_object_instantiate", kInstantiateSignature},
        patterns::Pattern{"placed_object_destroy", kDestroySignature},
        patterns::Pattern{"object_datum_allocate", kAllocateSignature},
        patterns::Pattern{"object_logical_destroy", kLogicalDestroySignature},
        patterns::Pattern{"object_handle_pair", kResolvePairSignature},
        patterns::Pattern{"object_handle_validate", kValidatePairSignature},
        patterns::Pattern{"object_datum_layout", kDatumLayoutSignature},
    };
    std::array<patterns::Match, definitions.size()> matches{};
    if (!patterns::resolve_all(std::span(ranges.data(), main.count), definitions, matches)) {
        return false;
    }
    for (const patterns::Match& match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }
    output = {matches[0].address,
              matches[1].address,
              matches[2].address,
              matches[3].address,
              matches[4].address,
              matches[5].address,
              matches[6].address};
    return true;
}

/** Binds the two datum globals encoded at fixed operands in the checked layout signature. */
[[nodiscard]] bool bind_datum_layout(std::byte* target) noexcept {
    if (target == nullptr) {
        return false;
    }
    g_datumStrideStorage = reinterpret_cast<const std::uint32_t*>(
        patterns::resolve_relative(target + 41, target + 45));
    g_datumBaseStorage = reinterpret_cast<const std::uintptr_t*>(
        patterns::resolve_relative(target + 57, target + 61));
    return g_datumStrideStorage != nullptr && g_datumBaseStorage != nullptr;
}

} // namespace

/** Installs the generation-checked placed-object lifetime capture. */
bool install() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_handles[0].attached && g_handles[1].attached && g_handles[2].attached
        && g_handles[3].attached) {
        const bool accepting = g_accepting.load(std::memory_order_acquire);
        ReleaseSRWLockExclusive(&g_lock);
        return accepting;
    }
    if (g_handles[0].attached || g_handles[1].attached || g_handles[2].attached
        || g_handles[3].attached) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_moduleBase = main_module_base();
    Targets targets{};
    if (!resolve_targets(targets) || !bind_datum_layout(targets.datumLayout)) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=world_objects stage=install result=fail reason=targets");
        return false;
    }
    g_resolvePair = reinterpret_cast<ResolvePair>(targets.resolvePair);
    g_validatePair = reinterpret_cast<ValidatePair>(targets.validatePair);
    const std::array specs{
        hooking::detour::Spec{targets.instantiate, reinterpret_cast<void*>(&instantiate)},
        hooking::detour::Spec{targets.destroy, reinterpret_cast<void*>(&destroy)},
        hooking::detour::Spec{targets.allocate, reinterpret_cast<void*>(&allocate)},
        hooking::detour::Spec{targets.logicalDestroy, reinterpret_cast<void*>(&logical_destroy)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        g_resolvePair = nullptr;
        g_validatePair = nullptr;
        g_datumBaseStorage = nullptr;
        g_datumStrideStorage = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=world_objects stage=install result=fail reason=detour");
        return false;
    }
    g_instantiateOriginal.store(reinterpret_cast<Instantiate>(g_handles[0].original),
                                std::memory_order_release);
    g_destroyOriginal.store(reinterpret_cast<Destroy>(g_handles[1].original),
                            std::memory_order_release);
    g_allocateOriginal.store(reinterpret_cast<Allocate>(g_handles[2].original),
                             std::memory_order_release);
    g_logicalDestroyOriginal.store(reinterpret_cast<LogicalDestroy>(g_handles[3].original),
                                   std::memory_order_release);

    g_accepting.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=world_objects stage=install result=ok");
    return true;
}

/** Removes both lifetime hooks only after native calls have left their trampolines. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_handles[0].attached && !g_handles[1].attached && !g_handles[2].attached
        && !g_handles[3].attached) {
        clear_registry();
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    g_accepting.store(false, std::memory_order_release);
    const std::array<hooking::detour::ProtectedCodeEntry, 4> protectedEntries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&instantiate)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&destroy)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&allocate)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&logical_destroy)},
    };
    const hooking::detour::UninstallResult result =
        hooking::detour::uninstall(g_handles, protectedEntries, &calls_idle);
    if (result != hooking::detour::UninstallResult::removed) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_instantiateOriginal.store(nullptr, std::memory_order_release);
    g_destroyOriginal.store(nullptr, std::memory_order_release);
    g_allocateOriginal.store(nullptr, std::memory_order_release);
    g_logicalDestroyOriginal.store(nullptr, std::memory_order_release);
    g_resolvePair = nullptr;
    g_validatePair = nullptr;
    g_datumBaseStorage = nullptr;
    g_datumStrideStorage = nullptr;
    clear_registry();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** @return True while both hooks are attached and accepting observations. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool installed = g_handles[0].attached && g_handles[1].attached && g_handles[2].attached
                           && g_handles[3].attached && g_accepting.load(std::memory_order_acquire);
    ReleaseSRWLockShared(&g_lock);
    return installed;
}

/** Finds exact live instances, pruning any generation or datum mismatch. */
std::size_t
find(std::uint32_t objectListTag, std::uint32_t entryIndex, std::span<Instance> output) noexcept {
    std::size_t found = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (RegistryEntry& entry : g_entries) {
        if (entry.state != EntryState::live || entry.instance.objectListTag != objectListTag
            || entry.instance.entryIndex != entryIndex) {
            continue;
        }
        if (!validate(entry.instance)) {
            entry.state = EntryState::tombstone;
            --g_liveCount;
            continue;
        }
        if (found < output.size()) {
            output[found] = entry.instance;
        }
        ++found;
    }
    if (g_liveCount == 0) {
        clear_registry();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

/** @return Current bounded-registry counters. */
Diagnostics diagnostics() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Diagnostics result{g_liveCount,
                             g_overflowCount,
                             g_handles[0].attached && g_handles[1].attached
                                 && g_accepting.load(std::memory_order_acquire)};
    ReleaseSRWLockShared(&g_lock);
    return result;
}

} // namespace sunrise::client::hooks::world_objects
