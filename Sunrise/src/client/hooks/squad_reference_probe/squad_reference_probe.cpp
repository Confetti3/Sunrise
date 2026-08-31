#include "squad_reference_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../diagnostics/module_range.h"
#include "../../hooking/detour.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::squad_reference_probe {
namespace {

using patterns::signature;
using patterns::signature_length;

/** Unpacked build-86657 `AiSpawner_ResolveSquad` prologue at RVA 0x4E6480. */
constexpr std::string_view kResolveText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 44 8B 19 48 8B D9 41 8B C3";
constexpr auto kResolve = signature<signature_length(kResolveText)>(kResolveText);
constexpr std::uintptr_t kResolveRva = 0x4E6480;

/** Unpacked build-86657 `AiSquadRelay_ApplySenseState` prologue at RVA 0x4C9270. */
constexpr std::string_view kRelayText =
    "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 49 89 7B 20 41 56 48 83 EC 30";
constexpr auto kRelay = signature<signature_length(kRelayText)>(kRelayText);
constexpr std::uintptr_t kRelayRva = 0x4C9270;

/** Unpacked build-86657 `AiSpawner_Start` prologue at RVA 0x4E3C50. */
constexpr std::string_view kSpawnerStartText =
    "40 56 48 83 EC 40 83 B9 FC 05 00 00 FF 48 8B F1 0F 84 27 01 00 00";
constexpr auto kSpawnerStart =
    signature<signature_length(kSpawnerStartText)>(kSpawnerStartText);
constexpr std::uintptr_t kSpawnerStartRva = 0x4E3C50;

/** Unpacked build-86657 `AiSpawner_ApplySenseState` prologue at RVA 0x4E8FB0. */
constexpr std::string_view kSpawnerApplyText =
    "48 89 5C 24 10 57 48 83 EC 30 48 8B FA 48 8B D9 E8 9B F0 FF FF";
constexpr auto kSpawnerApply =
    signature<signature_length(kSpawnerApplyText)>(kSpawnerApplyText);
constexpr std::uintptr_t kSpawnerApplyRva = 0x4E8FB0;

/** Unpacked build-86657 `AiSpawner_BuildRequests` prologue at RVA 0x4E2E80. */
constexpr std::string_view kBuildRequestsText =
    "48 8B C4 48 89 58 10 48 89 70 18 55 57 41 54 41 56 41 57 48 8D A8 A8 F5 FF FF";
constexpr auto kBuildRequests =
    signature<signature_length(kBuildRequestsText)>(kBuildRequestsText);
constexpr std::uintptr_t kBuildRequestsRva = 0x4E2E80;

/** Build-86657 lower entity-create wrapper, recovered at RVA 0x170F190. */
constexpr std::string_view kSobjectCreateText =
    "40 55 53 56 57 41 56 48 8D AC 24 C0 FD FF FF 48 81 EC 40 03 00 00";
constexpr auto kSobjectCreate =
    signature<signature_length(kSobjectCreateText)>(kSobjectCreateText);
constexpr std::uintptr_t kSobjectCreateRva = 0x170F190;

/** Build-86657 entity-datum allocator called by the lower wrapper. */
constexpr std::string_view kSobjectAllocateText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B DA C7 02 FF FF FF FF";
constexpr auto kSobjectAllocate =
    signature<signature_length(kSobjectAllocateText)>(kSobjectAllocateText);
constexpr std::uintptr_t kSobjectAllocateRva = 0x1711D10;

/** Build-86657 entity initializer called after a datum is allocated. */
constexpr std::string_view kSobjectInitializeText =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 55 41 56 41 57 48 83 EC 20";
constexpr auto kSobjectInitialize =
    signature<signature_length(kSobjectInitializeText)>(kSobjectInitializeText);
constexpr std::uintptr_t kSobjectInitializeRva = 0x170B0F0;

constexpr std::size_t kSquadReferenceOffset = 0x1D0;
constexpr std::size_t kRequestedMembersOffset = 0x650;
constexpr std::size_t kPendingCountOffset = 0x268;
constexpr std::uint32_t kValidRecordLimit = 96;
constexpr std::uint32_t kInvalidRecordLimit = 32;
constexpr std::uint32_t kSpawnerRecordLimit = 256;
constexpr std::uint32_t kRequestRecordLimit = 32;
constexpr std::uint32_t kCreateRecordLimit = 64;
constexpr std::uint32_t kAllocatorBitmapRecordLimit = 8;
constexpr std::size_t kAllocatorBitmapOffset = 0xC118;
constexpr std::size_t kAllocatorBitmapBytes = 1024;
constexpr std::size_t kAllocatorBitmapBits = kAllocatorBitmapBytes * 8;
constexpr std::size_t kAllocatorStackDepth = 8;
constexpr std::uint64_t kCreateCorrelationWindowMs = 250;
constexpr std::uint64_t kInstallRetryMilliseconds = 5'000;

struct SquadReference {
    std::int32_t handle;
    std::int32_t unknown;
    std::uint64_t pointer;
};
static_assert(sizeof(SquadReference) == 0x10);

using Resolve = std::uintptr_t(__fastcall*)(const std::byte*,
                                             std::uint8_t,
                                             SquadReference*,
                                             SquadReference*);
using ApplyRelayState = std::uintptr_t(__fastcall*)(std::byte*, const std::byte*);
using SpawnerStart = std::uintptr_t(__fastcall*)(std::byte*);
using ApplySpawnerState = std::uintptr_t(__fastcall*)(std::byte*, const std::byte*);
using BuildRequests = std::uintptr_t(__fastcall*)(std::byte*,
                                                   std::int32_t,
                                                   const std::int32_t*,
                                                   std::byte*);
using SobjectCreate = std::int32_t*(__fastcall*)(std::byte*,
                                                 std::int32_t*,
                                                 std::int32_t,
                                                 std::int32_t,
                                                 std::int32_t);
using SobjectAllocate = std::int32_t*(__fastcall*)(std::byte*, std::int32_t*);
using SobjectInitialize = bool(__fastcall*)(std::byte*,
                                            std::int32_t,
                                            std::int32_t,
                                            std::int32_t,
                                            std::int32_t);

std::array<hooking::detour::Handle, 8> g_handles{};
SRWLOCK g_snapshotLock = SRWLOCK_INIT;
enum class Lifecycle : std::uint8_t { detached, installing, installed, stopping, stopped };
std::atomic<Lifecycle> g_lifecycle{Lifecycle::detached};
std::atomic<void*> g_resolveOriginal{};
std::atomic<void*> g_relayOriginal{};
std::atomic<void*> g_spawnerStartOriginal{};
std::atomic<void*> g_spawnerApplyOriginal{};
std::atomic<void*> g_buildRequestsOriginal{};
std::atomic<void*> g_sobjectCreateOriginal{};
std::atomic<void*> g_sobjectAllocateOriginal{};
std::atomic<void*> g_sobjectInitializeOriginal{};
std::atomic_uint32_t g_validRecordCount{};
std::atomic_uint32_t g_invalidRecordCount{};
std::atomic_uint32_t g_spawnerRecordCount{};
std::atomic_uint64_t g_requestCallCount{};
std::atomic_uint32_t g_requestLogCount{};
std::atomic_uint64_t g_createCallCount{};
std::atomic_uint32_t g_createLogCount{};
std::atomic_uint32_t g_allocatorBitmapLogCount{};
std::atomic_uint64_t g_lastBuildTick{};
std::atomic_uint64_t g_lastBuildInstance{};
std::atomic_uint64_t g_lastBuildResult{};
std::atomic_int32_t g_lastBuildProduced{-1};
std::atomic_int32_t g_lastBuildMode{-1};
std::atomic_int32_t g_lastBuildMemberCount{-1};
std::atomic_int32_t g_lastBuildFirst{-1};
std::atomic_int32_t g_lastBuildSecond{-1};
std::atomic_uint64_t g_nextInstallAttempt{};
std::atomic_uint64_t g_applyCalls{};
std::atomic_uint64_t g_resolveCalls{};
std::atomic_uint64_t g_lastActiveInstance{};
std::atomic_uint32_t g_lastRequestedFirst{};
std::atomic_uint32_t g_lastRequestedSecond{};
std::atomic_uint32_t g_lastPending{};
std::atomic_uint64_t g_decodedState{};
std::atomic_uint32_t g_decodedSlotCount{};
std::atomic_uint32_t g_decodedRequestedFirst{};
std::atomic_uint32_t g_decodedRequestedSecond{};
std::atomic_uint32_t g_decodedGeneration{};
std::atomic_uint8_t g_decodedMode{};
std::atomic_bool g_decodedActive{};
volatile LONG g_activeCalls{};

struct BuildContext final {
    std::uintptr_t instance{};
    std::int32_t mode{-1};
    std::int32_t memberCount{-1};
    std::int32_t first{-1};
    std::int32_t second{-1};
    bool active{};
};
thread_local BuildContext g_buildContext{};

class CallScope {
public:
    CallScope() noexcept { (void)InterlockedIncrement(&g_activeCalls); }
    ~CallScope() noexcept { (void)InterlockedDecrement(&g_activeCalls); }
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
};

[[nodiscard]] bool calls_idle() noexcept {
    return InterlockedCompareExchange(&g_activeCalls, 0, 0) == 0;
}

template <typename T>
[[nodiscard]] bool read_native(const T* input, T& output) noexcept {
    if (input == nullptr) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), input, &output, sizeof output, &copied) != FALSE
           && copied == sizeof output;
}

[[nodiscard]] bool claim_bounded(std::atomic_uint32_t& counter,
                                 std::uint32_t limit) noexcept {
    std::uint32_t count = counter.load(std::memory_order_relaxed);
    while (count < limit) {
        if (counter.compare_exchange_weak(
                count, count + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

struct AllocatorBitmapSnapshot final {
    std::uint32_t setBits{};
    std::int32_t firstSet{-1};
    std::uint64_t hash{};
    bool readable{};
};

[[nodiscard]] AllocatorBitmapSnapshot read_allocator_bitmap(const std::byte* context) noexcept {
    AllocatorBitmapSnapshot output{};
    if (context == nullptr) return output;
    std::array<std::byte, kAllocatorBitmapBytes> bytes{};
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(),
                          context + kAllocatorBitmapOffset,
                          bytes.data(), bytes.size(), &copied) == FALSE
        || copied != bytes.size()) return output;
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); ++byteIndex) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[byteIndex]);
        output.setBits += static_cast<std::uint32_t>(std::popcount(value));
        hash = (hash ^ value) * 1099511628211ULL;
        if (output.firstSet < 0 && value != 0) {
            output.firstSet = static_cast<std::int32_t>(
                byteIndex * 8 + std::countr_zero(value));
        }
    }
    output.hash = hash;
    output.readable = true;
    return output;
}

[[nodiscard]] bool correlate_build(const BuildContext& input,
                                   BuildContext& output,
                                   std::uint64_t& age,
                                   const char*& correlation) noexcept {
    output = input;
    age = 0;
    correlation = "synchronous";
    if (output.active) return true;
    const RuntimeSnapshot recent = runtime_snapshot();
    const std::uint64_t now = GetTickCount64();
    age = recent.lastBuildTick != 0 && now >= recent.lastBuildTick
              ? now - recent.lastBuildTick
              : ~std::uint64_t{};
    if (age > kCreateCorrelationWindowMs) return false;
    output = {recent.lastBuildInstance,
              recent.lastBuildMode,
              recent.lastBuildMemberCount,
              recent.lastBuildFirst,
              recent.lastBuildSecond,
              true};
    correlation = "recent";
    return true;
}

void report_create_stage(const char* stage,
                         const BuildContext& build,
                         const std::byte* context,
                         const std::int32_t* output,
                         std::int32_t datum,
                         std::int32_t argument3,
                         std::int32_t argument4,
                         std::int32_t argument5,
                         std::int32_t outcome) noexcept {
    BuildContext correlated{};
    std::uint64_t age = 0;
    const char* correlation = nullptr;
    if (!correlate_build(build, correlated, age, correlation)) return;
    g_createCallCount.fetch_add(1, std::memory_order_relaxed);
    if (!claim_bounded(g_createLogCount, kCreateRecordLimit)) return;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(),
        "ev=sobject_create_probe stage=%s correlation=%s build_age_ms=%llu "
        "build_instance=0x%llX build_mode=%d members=%d requested=%d,%d "
        "context=0x%llX output=0x%llX datum=0x%08X "
        "argument3=%d argument4=%d argument5=%d outcome=%d",
        stage, correlation, static_cast<unsigned long long>(age),
        static_cast<unsigned long long>(correlated.instance),
        correlated.mode, correlated.memberCount, correlated.first, correlated.second,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(output)),
        static_cast<unsigned>(datum), argument3, argument4, argument5, outcome);
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
}

void report_allocator_bitmap(const BuildContext& correlated,
                             std::uint64_t age,
                             const char* correlation,
                             const std::byte* context,
                             const AllocatorBitmapSnapshot& before,
                             const AllocatorBitmapSnapshot& after,
                             std::span<void* const> stack) noexcept {
    if (!correlated.active || correlation == nullptr
        || !claim_bounded(g_allocatorBitmapLogCount, kAllocatorBitmapRecordLimit)) return;
    client::diagnostics::ModuleRange image{};
    const bool imageValid = client::diagnostics::module_range(GetModuleHandleW(nullptr), image);
    std::array<char, 256> stackText{};
    std::size_t stackWritten = 0;
    for (std::size_t index = 0; index < stack.size(); ++index) {
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(stack[index]);
        const std::uintptr_t rva = imageValid && client::diagnostics::contains(image, address)
                                       ? address - image.base
                                       : 0;
        const int written = std::snprintf(stackText.data() + stackWritten,
                                          stackText.size() - stackWritten,
                                          "%s0x%llX",
                                          index == 0 ? "" : ",",
                                          static_cast<unsigned long long>(rva));
        if (written <= 0 || static_cast<std::size_t>(written) >= stackText.size() - stackWritten) {
            break;
        }
        stackWritten += static_cast<std::size_t>(written);
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(),
        "ev=sobject_create_probe stage=allocator_bitmap correlation=%s build_age_ms=%llu "
        "build_instance=0x%llX context=0x%llX bitmap=0x%llX capacity=%zu "
        "before_read=%u before_set=%u before_first=%d before_hash=0x%016llX "
        "after_read=%u after_set=%u after_first=%d after_hash=0x%016llX stack=%s",
        correlation,
        static_cast<unsigned long long>(age),
        static_cast<unsigned long long>(correlated.instance),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(context) + kAllocatorBitmapOffset),
        kAllocatorBitmapBits,
        before.readable ? 1U : 0U,
        before.setBits,
        before.firstSet,
        static_cast<unsigned long long>(before.hash),
        after.readable ? 1U : 0U,
        after.setBits,
        after.firstSet,
        static_cast<unsigned long long>(after.hash),
        stackText.data());
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
}

[[nodiscard]] std::byte* validated_target(
    std::uintptr_t rva,
    std::span<const patterns::PatternByte> expected) noexcept {
    auto* const base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr || expected.empty()) {
        return nullptr;
    }
    std::array<std::byte, 32> actual{};
    if (expected.size() > actual.size()) {
        return nullptr;
    }
    SIZE_T copied = 0;
    std::byte* const target = base + rva;
    if (ReadProcessMemory(
            GetCurrentProcess(), target, actual.data(), expected.size(), &copied)
            == FALSE
        || copied != expected.size()) {
        return nullptr;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index].exact && actual[index] != expected[index].value) {
            return nullptr;
        }
    }
    return target;
}

[[nodiscard]] bool claim_record(bool valid) noexcept {
    auto& counter = valid ? g_validRecordCount : g_invalidRecordCount;
    return claim_bounded(counter, valid ? kValidRecordLimit : kInvalidRecordLimit);
}

void report(const char* stage,
            const std::byte* instance,
            std::uint8_t flag,
            const SquadReference& reference,
            const char* output) noexcept {
    const bool valid = reference.handle >= 0 && reference.pointer != 0;
    if (!claim_record(valid)) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=squad_reference_probe stage=%s instance=0x%llX flag=%u output=%s "
        "handle=%d unknown=%d pointer=0x%016llX valid=%u",
        stage,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(instance)),
        static_cast<unsigned>(flag),
        output,
        reference.handle,
        reference.unknown,
        static_cast<unsigned long long>(reference.pointer),
        valid ? 1U : 0U);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void report_spawner(const char* stage,
                    const std::byte* instance,
                    const std::byte* message) noexcept {
    std::uint32_t count = g_spawnerRecordCount.load(std::memory_order_relaxed);
    while (count < kSpawnerRecordLimit) {
        if (g_spawnerRecordCount.compare_exchange_weak(
                count, count + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            break;
        }
    }
    if (count >= kSpawnerRecordLimit) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=squad_reference_probe stage=%s instance=0x%llX message=0x%llX",
        stage,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(instance)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(message)));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void report_spawner_state(const std::byte* instance) noexcept {
    if (instance == nullptr) return;
    const auto* requested =
        reinterpret_cast<const std::uint32_t*>(instance + kRequestedMembersOffset);
    std::uint32_t requestedFirst = 0;
    std::uint32_t requestedSecond = 0;
    std::uint32_t pending = 0;
    if (!read_native(requested, requestedFirst)
        || !read_native(requested + 1, requestedSecond)
        || !read_native(reinterpret_cast<const std::uint32_t*>(
                            instance + kPendingCountOffset), pending)) return;
    // Zero is the idle baseline for every instance. Reporting only a changed or pending spawner
    // keeps this research probe useful while roster refreshes apply to hundreds of components.
    if (requestedFirst == 0 && requestedSecond == 0 && pending == 0) return;
    AcquireSRWLockExclusive(&g_snapshotLock);
    g_lastRequestedFirst.store(requestedFirst, std::memory_order_relaxed);
    g_lastRequestedSecond.store(requestedSecond, std::memory_order_relaxed);
    g_lastPending.store(pending, std::memory_order_relaxed);
    g_lastActiveInstance.store(reinterpret_cast<std::uintptr_t>(instance),
                               std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_snapshotLock);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=squad_reference_probe stage=spawner_state instance=0x%llX requested=%u,%u pending=%u",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(instance)),
        requestedFirst,
        requestedSecond,
        pending);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void inspect_committed_state(const std::byte* instance) noexcept {
    if (instance == nullptr) return;
    const auto* payload = instance + 0x180;
    std::array<std::byte, 0xC4> state{};
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(), payload, state.data(), state.size(), &copied) == FALSE
        || copied != state.size()) return;
    const auto read32 = [&state](std::size_t offset) noexcept {
        std::uint32_t value = 0;
        std::memcpy(&value, state.data() + offset, sizeof value);
        return value;
    };
    const std::uint32_t count = read32(0x2C);
    if (count > 8) return;
    const std::uint32_t requestedFirst = read32(0x30);
    const std::uint32_t requestedSecond = read32(0x34);
    const std::uint32_t generation = read32(0x7C);
    const bool active = state[0xBC] != std::byte{};
    const std::uint8_t mode = static_cast<std::uint8_t>(state[0xBD]);
    // `message` is a `{schema,pointer}` wrapper. The native function resolves it, then commits the
    // resulting 0xC4-byte state here. Inspecting the wrapper itself reports plausible zeroes.
    if (count == 0 && requestedFirst == 0 && requestedSecond == 0 && generation == 0
        && !active && mode == 0) return;
    AcquireSRWLockExclusive(&g_snapshotLock);
    g_decodedState.store(reinterpret_cast<std::uintptr_t>(payload), std::memory_order_relaxed);
    g_decodedSlotCount.store(count, std::memory_order_relaxed);
    g_decodedRequestedFirst.store(requestedFirst, std::memory_order_relaxed);
    g_decodedRequestedSecond.store(requestedSecond, std::memory_order_relaxed);
    g_decodedGeneration.store(generation, std::memory_order_relaxed);
    g_decodedActive.store(active, std::memory_order_relaxed);
    g_decodedMode.store(mode, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_snapshotLock);

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=squad_reference_probe stage=decoded_state state=0x%llX count=%u "
        "requested=%u,%u generation=%u active=%u mode=%u",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(payload)),
        count,
        requestedFirst,
        requestedSecond,
        generation,
        active ? 1U : 0U,
        static_cast<unsigned>(mode));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

std::uintptr_t __fastcall resolve(const std::byte* instance,
                                  std::uint8_t flag,
                                  SquadReference* primary,
                                  SquadReference* secondary) noexcept {
    CallScope scope;
    void* target = g_resolveOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_resolveOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<Resolve>(target);
    if (original == nullptr) {
        return 0;
    }
    g_resolveCalls.fetch_add(1, std::memory_order_relaxed);
    const std::uintptr_t result = original(instance, flag, primary, secondary);
    if (primary != nullptr) {
        report("resolve", instance, flag, *primary, "primary");
    }
    if (secondary != nullptr) {
        report("resolve", instance, flag, *secondary, "secondary");
    }
    return result;
}

std::uintptr_t __fastcall apply_relay_state(std::byte* instance,
                                            const std::byte* message) noexcept {
    CallScope scope;
    void* target = g_relayOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_relayOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<ApplyRelayState>(target);
    if (original == nullptr) {
        return 0;
    }
    const std::uintptr_t result = original(instance, message);
    if (instance != nullptr) {
        const auto& reference =
            *reinterpret_cast<const SquadReference*>(instance + kSquadReferenceOffset);
        report("relay_apply", instance, 0, reference, "committed");
    }
    return result;
}

std::uintptr_t __fastcall spawner_start(std::byte* instance) noexcept {
    CallScope scope;
    void* target = g_spawnerStartOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_spawnerStartOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<SpawnerStart>(target);
    if (original == nullptr) {
        return 0;
    }
    report_spawner("spawner_start", instance, nullptr);
    return original(instance);
}

std::uintptr_t __fastcall apply_spawner_state(std::byte* instance,
                                              const std::byte* message) noexcept {
    CallScope scope;
    void* target = g_spawnerApplyOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_spawnerApplyOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<ApplySpawnerState>(target);
    if (original == nullptr) {
        return 0;
    }
    g_applyCalls.fetch_add(1, std::memory_order_relaxed);
    report_spawner("spawner_apply_begin", instance, message);
    const std::uintptr_t result = original(instance, message);
    inspect_committed_state(instance);
    report_spawner("spawner_apply_end", instance, message);
    report_spawner_state(instance);
    return result;
}

std::uintptr_t __fastcall build_requests(std::byte* instance,
                                         std::int32_t mode,
                                         const std::int32_t* requested,
                                         std::byte* output) noexcept {
    CallScope scope;
    void* target = g_buildRequestsOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_buildRequestsOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<BuildRequests>(target);
    if (original == nullptr) {
        return 0;
    }
    std::int32_t memberCount = -1;
    std::int32_t first = -1;
    std::int32_t second = -1;
    if (read_native(requested, memberCount) && memberCount >= 0) {
        if (memberCount > 0) static_cast<void>(read_native(requested + 1, first));
        if (memberCount > 1) static_cast<void>(read_native(requested + 2, second));
    }
    const BuildContext previous = g_buildContext;
    g_buildContext = {reinterpret_cast<std::uintptr_t>(instance),
                      mode, memberCount, first, second, true};
    const std::uintptr_t result = original(instance, mode, requested, output);
    g_buildContext = previous;
    std::int32_t produced = -1;
    static_cast<void>(read_native(reinterpret_cast<const std::int32_t*>(output), produced));
    AcquireSRWLockExclusive(&g_snapshotLock);
    g_lastBuildInstance.store(reinterpret_cast<std::uintptr_t>(instance),
                              std::memory_order_relaxed);
    g_lastBuildResult.store(result, std::memory_order_relaxed);
    g_lastBuildProduced.store(produced, std::memory_order_relaxed);
    g_lastBuildMode.store(mode, std::memory_order_relaxed);
    g_lastBuildMemberCount.store(memberCount, std::memory_order_relaxed);
    g_lastBuildFirst.store(first, std::memory_order_relaxed);
    g_lastBuildSecond.store(second, std::memory_order_relaxed);
    g_lastBuildTick.store(GetTickCount64(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_snapshotLock);
    g_requestCallCount.fetch_add(1, std::memory_order_relaxed);
    if (claim_bounded(g_requestLogCount, kRequestRecordLimit)) {
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=squad_reference_probe stage=build_requests instance=0x%llX "
            "mode=%d members=%d requested=%d,%d produced=%d result=0x%llX",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(instance)),
            mode,
            memberCount,
            first,
            second,
            produced,
            static_cast<unsigned long long>(result));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return result;
}

std::int32_t* __fastcall sobject_create(std::byte* context,
                                        std::int32_t* output,
                                        std::int32_t argument3,
                                        std::int32_t argument4,
                                        std::int32_t argument5) noexcept {
    CallScope scope;
    void* target = g_sobjectCreateOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_sobjectCreateOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<SobjectCreate>(target);
    if (original == nullptr) return output;
    const BuildContext build = g_buildContext;
    std::int32_t* const result =
        original(context, output, argument3, argument4, argument5);
    std::int32_t datum = -1;
    static_cast<void>(read_native(result, datum));
    report_create_stage("lower_create",
                        build, context, result, datum,
                        argument3, argument4, argument5,
                        datum == -1 ? 0 : 1);
    return result;
}

std::int32_t* __fastcall sobject_allocate(std::byte* context,
                                          std::int32_t* output) noexcept {
    CallScope scope;
    void* target = g_sobjectAllocateOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_sobjectAllocateOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<SobjectAllocate>(target);
    if (original == nullptr) return output;
    const BuildContext build = g_buildContext;
    BuildContext bitmapBuild{};
    std::uint64_t bitmapAge = 0;
    const char* bitmapCorrelation = nullptr;
    const bool captureBitmap =
        g_allocatorBitmapLogCount.load(std::memory_order_relaxed)
            < kAllocatorBitmapRecordLimit
        && correlate_build(build, bitmapBuild, bitmapAge, bitmapCorrelation);
    const AllocatorBitmapSnapshot before =
        captureBitmap ? read_allocator_bitmap(context) : AllocatorBitmapSnapshot{};
    std::array<void*, kAllocatorStackDepth> stack{};
    const USHORT stackCount = captureBitmap
                                  ? RtlCaptureStackBackTrace(
                                        0, static_cast<DWORD>(stack.size()), stack.data(), nullptr)
                                  : 0;
    std::int32_t* const result = original(context, output);
    const AllocatorBitmapSnapshot after =
        captureBitmap ? read_allocator_bitmap(context) : AllocatorBitmapSnapshot{};
    std::int32_t datum = -1;
    static_cast<void>(read_native(result, datum));
    report_create_stage("allocate_datum",
                        build, context, result, datum,
                        -1, -1, -1,
                        datum == -1 ? 0 : 1);
    if (captureBitmap) {
        report_allocator_bitmap(
            bitmapBuild, bitmapAge, bitmapCorrelation, context, before, after,
            std::span<void* const>(stack.data(), static_cast<std::size_t>(stackCount)));
    }
    return result;
}

bool __fastcall sobject_initialize(std::byte* context,
                                   std::int32_t datum,
                                   std::int32_t argument3,
                                   std::int32_t argument4,
                                   std::int32_t argument5) noexcept {
    CallScope scope;
    void* target = g_sobjectInitializeOriginal.load(std::memory_order_acquire);
    while (target == nullptr
           && g_lifecycle.load(std::memory_order_acquire) == Lifecycle::installing) {
        YieldProcessor();
        target = g_sobjectInitializeOriginal.load(std::memory_order_acquire);
    }
    auto* original = reinterpret_cast<SobjectInitialize>(target);
    if (original == nullptr) return false;
    const BuildContext build = g_buildContext;
    const bool result = original(context, datum, argument3, argument4, argument5);
    report_create_stage("initialize_entity",
                        build, context, nullptr, datum,
                        argument3, argument4, argument5,
                        result ? 1 : 0);
    return result;
}

[[nodiscard]] bool fail(const char* stage, const char* reason) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=squad_reference_probe stage=%s result=fail reason=%s",
                                      stage,
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

void initialize() noexcept {
    Lifecycle expected = Lifecycle::stopped;
    (void)g_lifecycle.compare_exchange_strong(
        expected, Lifecycle::detached, std::memory_order_acq_rel);
    g_nextInstallAttempt.store(0, std::memory_order_release);
}

bool install() noexcept {
    Lifecycle expected = Lifecycle::detached;
    if (!g_lifecycle.compare_exchange_strong(
            expected, Lifecycle::installing, std::memory_order_acq_rel)) {
        return expected == Lifecycle::installed;
    }
    std::byte* const resolveTarget = validated_target(kResolveRva, kResolve);
    if (resolveTarget == nullptr) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "resolve_target");
    }
    std::byte* const relayTarget = validated_target(kRelayRva, kRelay);
    if (relayTarget == nullptr) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "relay_target");
    }
    std::byte* const spawnerStartTarget = validated_target(kSpawnerStartRva, kSpawnerStart);
    if (spawnerStartTarget == nullptr) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "spawner_start_target");
    }
    std::byte* const spawnerApplyTarget = validated_target(kSpawnerApplyRva, kSpawnerApply);
    if (spawnerApplyTarget == nullptr) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "spawner_apply_target");
    }
    std::byte* const buildRequestsTarget = validated_target(kBuildRequestsRva, kBuildRequests);
    if (buildRequestsTarget == nullptr) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "build_requests_target");
    }
    std::byte* const sobjectCreateTarget = validated_target(kSobjectCreateRva, kSobjectCreate);
    std::byte* const sobjectAllocateTarget =
        validated_target(kSobjectAllocateRva, kSobjectAllocate);
    std::byte* const sobjectInitializeTarget =
        validated_target(kSobjectInitializeRva, kSobjectInitialize);
    if (sobjectCreateTarget == nullptr || sobjectAllocateTarget == nullptr
        || sobjectInitializeTarget == nullptr) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "sobject_create_targets");
    }
    const std::array<hooking::detour::Spec, 8> specs{{
        {resolveTarget, reinterpret_cast<void*>(&resolve)},
        {relayTarget, reinterpret_cast<void*>(&apply_relay_state)},
        {spawnerStartTarget, reinterpret_cast<void*>(&spawner_start)},
        {spawnerApplyTarget, reinterpret_cast<void*>(&apply_spawner_state)},
        {buildRequestsTarget, reinterpret_cast<void*>(&build_requests)},
        {sobjectCreateTarget, reinterpret_cast<void*>(&sobject_create)},
        {sobjectAllocateTarget, reinterpret_cast<void*>(&sobject_allocate)},
        {sobjectInitializeTarget, reinterpret_cast<void*>(&sobject_initialize)},
    }};
    g_validRecordCount.store(0, std::memory_order_release);
    g_invalidRecordCount.store(0, std::memory_order_release);
    g_spawnerRecordCount.store(0, std::memory_order_release);
    g_requestCallCount.store(0, std::memory_order_release);
    g_requestLogCount.store(0, std::memory_order_release);
    g_createCallCount.store(0, std::memory_order_release);
    g_createLogCount.store(0, std::memory_order_release);
    g_allocatorBitmapLogCount.store(0, std::memory_order_release);
    AcquireSRWLockExclusive(&g_snapshotLock);
    g_lastBuildTick.store(0, std::memory_order_relaxed);
    g_lastBuildInstance.store(0, std::memory_order_release);
    g_lastBuildResult.store(0, std::memory_order_release);
    g_lastBuildProduced.store(-1, std::memory_order_release);
    g_lastBuildMode.store(-1, std::memory_order_release);
    g_lastBuildMemberCount.store(-1, std::memory_order_release);
    g_lastBuildFirst.store(-1, std::memory_order_release);
    g_lastBuildSecond.store(-1, std::memory_order_release);
    g_applyCalls.store(0, std::memory_order_release);
    g_resolveCalls.store(0, std::memory_order_release);
    g_lastActiveInstance.store(0, std::memory_order_release);
    g_lastRequestedFirst.store(0, std::memory_order_release);
    g_lastRequestedSecond.store(0, std::memory_order_release);
    g_lastPending.store(0, std::memory_order_release);
    g_decodedState.store(0, std::memory_order_relaxed);
    g_decodedSlotCount.store(0, std::memory_order_relaxed);
    g_decodedRequestedFirst.store(0, std::memory_order_relaxed);
    g_decodedRequestedSecond.store(0, std::memory_order_relaxed);
    g_decodedGeneration.store(0, std::memory_order_relaxed);
    g_decodedMode.store(0, std::memory_order_relaxed);
    g_decodedActive.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_snapshotLock);
    if (!hooking::detour::install(specs, g_handles)) {
        g_lifecycle.store(Lifecycle::detached, std::memory_order_release);
        return fail("install", "attach");
    }
    g_resolveOriginal.store(g_handles[0].original, std::memory_order_release);
    g_relayOriginal.store(g_handles[1].original, std::memory_order_release);
    g_spawnerStartOriginal.store(g_handles[2].original, std::memory_order_release);
    g_spawnerApplyOriginal.store(g_handles[3].original, std::memory_order_release);
    g_buildRequestsOriginal.store(g_handles[4].original, std::memory_order_release);
    g_sobjectCreateOriginal.store(g_handles[5].original, std::memory_order_release);
    g_sobjectAllocateOriginal.store(g_handles[6].original, std::memory_order_release);
    g_sobjectInitializeOriginal.store(g_handles[7].original, std::memory_order_release);
    g_lifecycle.store(Lifecycle::installed, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=squad_reference_probe stage=install result=ok");
    return true;
}

void service(std::uint64_t now) noexcept {
    if (g_lifecycle.load(std::memory_order_acquire) != Lifecycle::detached
        || !core::settings::get().server.activation.trostlandSpawnerProbe) {
        return;
    }
    std::uint64_t next = g_nextInstallAttempt.load(std::memory_order_relaxed);
    if (now < next
        || !g_nextInstallAttempt.compare_exchange_strong(
            next, now + kInstallRetryMilliseconds, std::memory_order_acq_rel)) {
        return;
    }
    (void)install();
}

RuntimeSnapshot runtime_snapshot() noexcept {
    RuntimeSnapshot output{};
    output.applyCalls = g_applyCalls.load(std::memory_order_relaxed);
    output.resolveCalls = g_resolveCalls.load(std::memory_order_relaxed);
    output.buildRequestCalls = g_requestCallCount.load(std::memory_order_relaxed);
    output.createOutcomeCalls = g_createCallCount.load(std::memory_order_relaxed);
    AcquireSRWLockShared(&g_snapshotLock);
    output.lastActiveInstance = static_cast<std::uintptr_t>(
        g_lastActiveInstance.load(std::memory_order_relaxed));
    output.requestedFirst = g_lastRequestedFirst.load(std::memory_order_relaxed);
    output.requestedSecond = g_lastRequestedSecond.load(std::memory_order_relaxed);
    output.pending = g_lastPending.load(std::memory_order_relaxed);
    output.lastBuildTick = g_lastBuildTick.load(std::memory_order_relaxed);
    output.lastBuildInstance = static_cast<std::uintptr_t>(
        g_lastBuildInstance.load(std::memory_order_relaxed));
    output.lastBuildResult = static_cast<std::uintptr_t>(
        g_lastBuildResult.load(std::memory_order_relaxed));
    output.lastBuildProduced = g_lastBuildProduced.load(std::memory_order_relaxed);
    output.lastBuildMode = g_lastBuildMode.load(std::memory_order_relaxed);
    output.lastBuildMemberCount = g_lastBuildMemberCount.load(std::memory_order_relaxed);
    output.lastBuildFirst = g_lastBuildFirst.load(std::memory_order_relaxed);
    output.lastBuildSecond = g_lastBuildSecond.load(std::memory_order_relaxed);
    output.decodedState = static_cast<std::uintptr_t>(
        g_decodedState.load(std::memory_order_relaxed));
    output.decodedSlotCount = g_decodedSlotCount.load(std::memory_order_relaxed);
    output.decodedRequestedFirst = g_decodedRequestedFirst.load(std::memory_order_relaxed);
    output.decodedRequestedSecond = g_decodedRequestedSecond.load(std::memory_order_relaxed);
    output.decodedGeneration = g_decodedGeneration.load(std::memory_order_relaxed);
    output.decodedMode = g_decodedMode.load(std::memory_order_relaxed);
    output.decodedActive = g_decodedActive.load(std::memory_order_relaxed);
    ReleaseSRWLockShared(&g_snapshotLock);
    return output;
}

bool uninstall() noexcept {
    Lifecycle lifecycle = g_lifecycle.load(std::memory_order_acquire);
    for (;;) {
        if (lifecycle == Lifecycle::stopped) {
            return true;
        }
        if (lifecycle == Lifecycle::installing || lifecycle == Lifecycle::stopping) {
            return false;
        }
        const Lifecycle next = lifecycle == Lifecycle::detached ? Lifecycle::stopped
                                                                 : Lifecycle::stopping;
        if (g_lifecycle.compare_exchange_weak(
                lifecycle, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (next == Lifecycle::stopped) {
                return true;
            }
            break;
        }
    }
    const std::array<hooking::detour::ProtectedCodeEntry, 8> protectedEntries{{
        {reinterpret_cast<void*>(&resolve)},
        {reinterpret_cast<void*>(&apply_relay_state)},
        {reinterpret_cast<void*>(&spawner_start)},
        {reinterpret_cast<void*>(&apply_spawner_state)},
        {reinterpret_cast<void*>(&build_requests)},
        {reinterpret_cast<void*>(&sobject_create)},
        {reinterpret_cast<void*>(&sobject_allocate)},
        {reinterpret_cast<void*>(&sobject_initialize)},
    }};
    const bool detached = hooking::detour::uninstall(g_handles, protectedEntries, calls_idle)
        == hooking::detour::UninstallResult::removed;
    if (detached) {
        g_resolveOriginal.store(nullptr, std::memory_order_release);
        g_relayOriginal.store(nullptr, std::memory_order_release);
        g_spawnerStartOriginal.store(nullptr, std::memory_order_release);
        g_spawnerApplyOriginal.store(nullptr, std::memory_order_release);
        g_buildRequestsOriginal.store(nullptr, std::memory_order_release);
        g_sobjectCreateOriginal.store(nullptr, std::memory_order_release);
        g_sobjectAllocateOriginal.store(nullptr, std::memory_order_release);
        g_sobjectInitializeOriginal.store(nullptr, std::memory_order_release);
        g_lifecycle.store(Lifecycle::stopped, std::memory_order_release);
    } else {
        g_lifecycle.store(Lifecycle::installed, std::memory_order_release);
    }
    return detached;
}

} // namespace sunrise::client::hooks::squad_reference_probe
