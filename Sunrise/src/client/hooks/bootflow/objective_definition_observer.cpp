#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/** Build-86657 objective-definition resolver ported instruction-for-instruction from build 87221. */
constexpr std::string_view kResolverSignatureText =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 C0 41 0F B7 "
    "D8 48 89 02 4C 8B F2 48 89 42 08 48 8B F1 E8 ? ? ? ? 48 8B CE 48 8B F8 E8 ? ? ? "
    "? 4C 8B 07 0F B7 D3 48 8B CF 48 8B F0 41 FF 90 A0 05 00 00 0F B7 D3 48 8B CE 49 "
    "89 06 E8 ? ? ? ?";
constexpr auto kResolverSignature =
    signature<signature_length(kResolverSignatureText)>(kResolverSignatureText);

constexpr std::size_t kRecordWords = 4;
constexpr std::uint32_t kReportLimit = 256;
constexpr std::size_t kLineCapacity = 320;
constexpr std::uint64_t kOccupiedBit = 0x8000'0000'0000'0000ULL;

struct OutputPair {
    const void* primary{};
    const void* secondary{};
};

struct Snapshot {
    bool primaryPresent{};
    bool secondaryPresent{};
    std::array<std::uint32_t, kRecordWords> primary{};
    std::array<std::uint32_t, kRecordWords> secondary{};
};

using Resolver = void*(__fastcall*)(void*, OutputPair*, std::uint16_t) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<Resolver> g_original{nullptr};
std::atomic<std::uint32_t> g_reports{};
std::array<std::atomic<std::uint64_t>, kReportLimit> g_seen{};

/** Claims a caller/index pair once so polling UI cannot exhaust the diagnostic budget. */
[[nodiscard]] bool claim(std::uintptr_t callerRva, std::uint16_t objectiveIndex) noexcept {
    const std::uint64_t key = kOccupiedBit | (static_cast<std::uint64_t>(callerRva) << 16U)
                              | static_cast<std::uint64_t>(objectiveIndex);
    std::size_t slot = static_cast<std::size_t>((key ^ (key >> 32U)) % g_seen.size());
    for (std::size_t searched = 0; searched < g_seen.size(); ++searched) {
        std::uint64_t observed = g_seen[slot].load(std::memory_order_acquire);
        if (observed == key) {
            return false;
        }
        if (observed == 0
            && g_seen[slot].compare_exchange_strong(
                observed, key, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
        slot = (slot + 1U) % g_seen.size();
    }
    return false;
}

/** Copies a bounded, pointer-free prefix from each definition record while the call owns it. */
[[nodiscard]] bool snapshot(const OutputPair* pair, Snapshot& output) noexcept {
    if (pair == nullptr) {
        return false;
    }
    __try {
        const OutputPair local = *pair;
        output.primaryPresent = local.primary != nullptr;
        output.secondaryPresent = local.secondary != nullptr;
        if (local.primary != nullptr) {
            std::memcpy(output.primary.data(), local.primary, sizeof output.primary);
        }
        if (local.secondary != nullptr) {
            std::memcpy(output.secondary.data(), local.secondary, sizeof output.secondary);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Emits one bounded objective-definition lookup observation. */
void report(std::uintptr_t callerRva, std::uint16_t objectiveIndex, const Snapshot& value) noexcept {
    if (!claim(callerRva, objectiveIndex)) {
        return;
    }
    const std::uint32_t sequence = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > kReportLimit) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=objective_definition stage=lookup n=%u caller=+0x%llX index=%u present=%u,%u "
        "primary=%08X,%08X,%08X,%08X secondary=%08X,%08X,%08X,%08X",
        sequence,
        static_cast<unsigned long long>(callerRva),
        static_cast<unsigned>(objectiveIndex),
        value.primaryPresent ? 1U : 0U,
        value.secondaryPresent ? 1U : 0U,
        value.primary[0],
        value.primary[1],
        value.primary[2],
        value.primary[3],
        value.secondary[0],
        value.secondary[1],
        value.secondary[2],
        value.secondary[3]);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Mirrors the native resolver and observes its completed output pair without changing it. */
__declspec(noinline) void* __fastcall resolve(void* manager,
                                               OutputPair* output,
                                               std::uint16_t objectiveIndex) noexcept {
    const auto* const caller = static_cast<const std::byte*>(_ReturnAddress());
    const auto* const image =
        static_cast<const std::byte*>(static_cast<const void*>(GetModuleHandleW(nullptr)));
    const std::uintptr_t callerRva =
        image != nullptr && caller >= image ? static_cast<std::uintptr_t>(caller - image) : 0;
    const Resolver original = g_original.load(std::memory_order_acquire);
    void* const result = original != nullptr ? original(manager, output, objectiveIndex) : output;
    Snapshot value{};
    if (snapshot(output, value)) {
        report(callerRva, objectiveIndex, value);
    }
    return result;
}

[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=objective_definition stage=install result=fail reason=%s",
                                      reason);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), length});
    }
    return false;
}

} // namespace

/** Attaches the read-only build-86657 objective-definition resolver observer. */
bool install_objective_definition_observer() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target =
        scan_main_image_unique(kResolverSignature, "objective_definition_resolver");
    if (target == nullptr) {
        return fail("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&resolve)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    g_original.store(reinterpret_cast<Resolver>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=objective_definition stage=install result=ok rva=+0xC923A0");
    return true;
}

/** Detaches the objective-definition resolver observer. */
void uninstall_objective_definition_observer() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reports.store(0, std::memory_order_release);
    for (auto& entry : g_seen) {
        entry.store(0, std::memory_order_release);
    }
}

} // namespace sunrise::client::hooks::bootflow
