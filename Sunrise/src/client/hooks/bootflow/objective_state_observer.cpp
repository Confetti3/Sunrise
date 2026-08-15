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

/** Build-86657 objective-completion reader ported exactly from build 87221. */
constexpr std::string_view kCompletionSignatureText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 "
    "48 8B 01 49 8B F1 49 8B E8 4C 8B F2 48 8B F9 FF 90 18 01 00 00 48 85 C0 74 0E "
    "48 8B 18 48 85 DB 74 06 48 8B 5B 08 EB 02 33 DB 48 8B CF E8 ? ? ? ?";
constexpr auto kCompletionSignature =
    signature<signature_length(kCompletionSignatureText)>(kCompletionSignatureText);

/** Build-86657 objective-progress reader ported exactly from build 87221. */
constexpr std::string_view kProgressSignatureText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 02 48 8B F9 "
    "48 8B CA 49 8B F1 49 8B E8 48 8B DA FF 90 18 01 00 00 48 85 C0 74 0E 48 8B 08 "
    "48 85 C9 74 06 48 8B 49 08 EB 02 33 C9 4C 8D 4C 24 30 45 33 C0 33 D2 E8 ? ? ? ?";
constexpr auto kProgressSignature =
    signature<signature_length(kProgressSignatureText)>(kProgressSignatureText);

constexpr std::size_t kDefinitionMaximumOffset = 0x30;
constexpr std::size_t kSeenCapacity = 256;
constexpr std::size_t kLineCapacity = 224;
constexpr std::uint64_t kOccupiedBit = 0x8000'0000'0000'0000ULL;

using Completion = bool(__fastcall*)(void*, const void*, const void*, const void*) noexcept;
using Progress = void*(__fastcall*)(
    void*, void*, const void*, const void*, const void*) noexcept;

hooking::detour::Handle g_completionHandle{};
hooking::detour::Handle g_progressHandle{};
std::atomic<Completion> g_completionOriginal{nullptr};
std::atomic<Progress> g_progressOriginal{nullptr};
std::array<std::atomic<std::uint64_t>, kSeenCapacity> g_seen{};
std::atomic<std::uint32_t> g_reports{};

struct DefinitionSnapshot {
    std::uint32_t hash{};
    std::uint32_t maximum{};
    std::uint8_t flag28{};
    std::uint8_t flag2c{};
};

/** Copies only fields consumed by the native completion/progress readers. */
[[nodiscard]] bool snapshot_definition(const void* definition,
                                       DefinitionSnapshot& output) noexcept {
    if (definition == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(definition);
        std::memcpy(&output.hash, bytes, sizeof output.hash);
        std::memcpy(&output.maximum,
                    bytes + kDefinitionMaximumOffset,
                    sizeof output.maximum);
        std::memcpy(&output.flag28, bytes + 0x28, sizeof output.flag28);
        std::memcpy(&output.flag2c, bytes + 0x2C, sizeof output.flag2c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Reads the progress result's first dword, which the build-87221 quest reader prints. */
[[nodiscard]] bool snapshot_progress(const void* result, std::uint32_t& output) noexcept {
    if (result == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&output, result, sizeof output);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] std::uintptr_t caller_rva(const void* returnAddress) noexcept {
    const auto* const caller = static_cast<const std::byte*>(returnAddress);
    const auto* const image =
        static_cast<const std::byte*>(static_cast<const void*>(GetModuleHandleW(nullptr)));
    return image != nullptr && caller >= image ? static_cast<std::uintptr_t>(caller - image) : 0;
}

/** Claims one observed state tuple so polling consumers emit again only when state changes. */
[[nodiscard]] bool claim(std::uint64_t key) noexcept {
    key |= kOccupiedBit;
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

void report(const char* operation,
            std::uintptr_t callerRva,
            const DefinitionSnapshot& definition,
            std::uint32_t value,
            bool contextPresent) noexcept {
    const std::uint64_t kind = operation[0] == 'p' ? 1ULL : 0ULL;
    const std::uint64_t key = (kind << 62U) ^ (static_cast<std::uint64_t>(callerRva) << 32U)
                              ^ (static_cast<std::uint64_t>(definition.hash) << 1U)
                              ^ static_cast<std::uint64_t>(value);
    if (!claim(key)) {
        return;
    }
    const std::uint32_t sequence = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=objective_state stage=%s n=%u caller=+0x%llX hash=%08X value=%u maximum=%u "
        "flags=%u,%u context=%u",
        operation,
        sequence,
        static_cast<unsigned long long>(callerRva),
        definition.hash,
        value,
        definition.maximum,
        static_cast<unsigned>(definition.flag28),
        static_cast<unsigned>(definition.flag2c),
        contextPresent ? 1U : 0U);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Mirrors the native completion reader and records its scalar result. */
__declspec(noinline) bool __fastcall completion(void* owner,
                                                 const void* definition,
                                                 const void* state,
                                                 const void* context) noexcept {
    const std::uintptr_t callerRva = caller_rva(_ReturnAddress());
    const Completion original = g_completionOriginal.load(std::memory_order_acquire);
    const bool result = original != nullptr ? original(owner, definition, state, context) : false;
    DefinitionSnapshot snapshot{};
    if (snapshot_definition(definition, snapshot)) {
        report("complete", callerRva, snapshot, result ? 1U : 0U, context != nullptr);
    }
    return result;
}

/** Mirrors the native progress reader, including its fifth stack argument. */
__declspec(noinline) void* __fastcall progress(void* output,
                                                void* owner,
                                                const void* definition,
                                                const void* context,
                                                const void* state) noexcept {
    const std::uintptr_t callerRva = caller_rva(_ReturnAddress());
    const Progress original = g_progressOriginal.load(std::memory_order_acquire);
    void* const result = original != nullptr
                             ? original(output, owner, definition, context, state)
                             : output;
    DefinitionSnapshot definitionSnapshot{};
    std::uint32_t current = 0;
    if (snapshot_definition(definition, definitionSnapshot)
        && snapshot_progress(result, current)) {
        report("progress", callerRva, definitionSnapshot, current, context != nullptr);
    }
    return result;
}

void log_install(const char* result, const char* reason = nullptr) noexcept {
    std::array<char, 144> line{};
    const int written = reason == nullptr
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=objective_state stage=install result=%s "
                                            "completion=+0x5269D0 progress=+0x523F30",
                                            result)
                            : std::snprintf(line.data(),
                                            line.size(),
                                            "ev=objective_state stage=install result=%s reason=%s",
                                            result,
                                            reason);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         reason == nullptr ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), length});
    }
}

} // namespace

/** Attaches the paired read-only objective completion/progress observers. */
bool install_objective_state_observer() noexcept {
    if (g_completionHandle.attached && g_progressHandle.attached) {
        return true;
    }
    std::byte* const completionTarget =
        scan_main_image_unique(kCompletionSignature, "objective_completion_reader");
    std::byte* const progressTarget =
        scan_main_image_unique(kProgressSignature, "objective_progress_reader");
    if (completionTarget == nullptr || progressTarget == nullptr) {
        log_install("fail", completionTarget == nullptr ? "completion-target" : "progress-target");
        return false;
    }
    if (!hooking::detour::install(
            {completionTarget, reinterpret_cast<void*>(&completion)}, g_completionHandle)) {
        log_install("fail", "completion-attach");
        return false;
    }
    g_completionOriginal.store(reinterpret_cast<Completion>(g_completionHandle.original),
                               std::memory_order_release);
    if (!hooking::detour::install(
            {progressTarget, reinterpret_cast<void*>(&progress)}, g_progressHandle)) {
        (void)hooking::detour::uninstall(g_completionHandle);
        g_completionOriginal.store(nullptr, std::memory_order_release);
        log_install("fail", "progress-attach");
        return false;
    }
    g_progressOriginal.store(reinterpret_cast<Progress>(g_progressHandle.original),
                             std::memory_order_release);
    log_install("ok");
    return true;
}

/** Detaches both objective-state readers. */
void uninstall_objective_state_observer() noexcept {
    if (g_progressHandle.attached) {
        (void)hooking::detour::uninstall(g_progressHandle);
    }
    if (g_completionHandle.attached) {
        (void)hooking::detour::uninstall(g_completionHandle);
    }
    g_progressOriginal.store(nullptr, std::memory_order_release);
    g_completionOriginal.store(nullptr, std::memory_order_release);
    g_reports.store(0, std::memory_order_release);
    for (auto& entry : g_seen) {
        entry.store(0, std::memory_order_release);
    }
}

} // namespace sunrise::client::hooks::bootflow
