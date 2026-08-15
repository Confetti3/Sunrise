#include <Windows.h>

#include <algorithm>
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
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/** Build-86657 category dispatcher that copies retained state rows into a rebuilt activity bank. */
constexpr std::string_view kApplySignatureText =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 70 48 63 84 24 C8 00 00 00 49 8B F1 "
    "49 8B F8 4C 8B CA 4C 8B D9 83 F8 0D";
constexpr auto kApplySignature =
    signature<signature_length(kApplySignatureText)>(kApplySignatureText);

/** Build-86657 old/new retained-state comparison callback reached after generic decode. */
constexpr std::string_view kCompareSignatureText =
    "48 89 5C 24 18 55 56 57 48 83 EC 30 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 28 "
    "48 8B E9 48 8B F2 48 8D 0D ? ? ? ? E8 ? ? ? ? 4C 8B 06";
constexpr auto kCompareSignature =
    signature<signature_length(kCompareSignatureText)>(kCompareSignatureText);

constexpr std::size_t kSwitchCountOffset = 0xB3F4;
constexpr std::size_t kSwitchRowsOffset = 0xB3F8;
constexpr std::size_t kSwitchRowStride = 4;
constexpr std::size_t kSwitchCapacity = 20;
constexpr std::size_t kProgressCountOffset = 0xB448;
constexpr std::size_t kProgressRowsOffset = 0xB44C;
constexpr std::size_t kProgressRowStride = 8;
constexpr std::size_t kProgressCaptureCapacity = 32;
constexpr std::size_t kSeenCapacity = 256;
constexpr std::size_t kReportCapacity = 192;
constexpr std::size_t kLineCapacity = 240;
constexpr std::uint64_t kOccupiedBit = 0x8000'0000'0000'0000ULL;

using ApplyCategory = void(__fastcall*)(void*,
                                        void*,
                                        void*,
                                        const void*,
                                        void*,
                                        void*,
                                        void*,
                                        void*,
                                        void*,
                                        std::int32_t,
                                        std::uintptr_t,
                                        void*,
                                        void*,
                                        void*) noexcept;
using CompareSource = void(__fastcall*)(void*, const void*) noexcept;

hooking::detour::Handle g_applyHandle{};
hooking::detour::Handle g_compareHandle{};
std::atomic<ApplyCategory> g_original{nullptr};
std::atomic<CompareSource> g_originalCompare{nullptr};
std::array<std::atomic<std::uint64_t>, kSeenCapacity> g_seen{};
std::atomic<std::uint32_t> g_reports{};

struct SwitchRow {
    std::int16_t index{};
    std::uint8_t value{};
    std::uint8_t auxiliary{};
};

struct ProgressRow {
    std::int16_t index{};
    std::uint16_t auxiliary{};
    std::uint32_t value{};
};

struct SourceSnapshot {
    std::array<std::uint32_t, 4> prefixWords{};
    std::uintptr_t vtableRva{};
    std::int32_t switchCount{};
    std::int32_t progressCount{};
    std::size_t capturedSwitches{};
    std::size_t capturedProgressions{};
    std::array<SwitchRow, kSwitchCapacity> switches{};
    std::array<ProgressRow, kProgressCaptureCapacity> progressions{};
};

/** Returns a stable main-image-relative value when one copied pointer identifies a client vtable.
 */
[[nodiscard]] std::uintptr_t main_image_rva(std::uintptr_t value) noexcept {
    const auto* const image =
        static_cast<const std::byte*>(static_cast<const void*>(GetModuleHandleW(nullptr)));
    if (image == nullptr || value < reinterpret_cast<std::uintptr_t>(image)) {
        return 0;
    }
    __try {
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return 0;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return 0;
        }
        const std::uintptr_t delta = value - reinterpret_cast<std::uintptr_t>(image);
        return delta < nt->OptionalHeader.SizeOfImage ? delta : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/** Copies bounded scalar rows before the native dispatcher consumes the retained source object. */
[[nodiscard]] bool snapshot_source(const void* source, SourceSnapshot& output) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(source);
        std::memcpy(output.prefixWords.data(), bytes, sizeof output.prefixWords);
        std::uintptr_t possibleVtable{};
        std::memcpy(&possibleVtable, bytes, sizeof possibleVtable);
        output.vtableRva = main_image_rva(possibleVtable);
        std::memcpy(&output.switchCount, bytes + kSwitchCountOffset, sizeof output.switchCount);
        std::memcpy(
            &output.progressCount, bytes + kProgressCountOffset, sizeof output.progressCount);
        if (output.switchCount > 0) {
            output.capturedSwitches = std::min<std::size_t>(
                static_cast<std::size_t>(output.switchCount), output.switches.size());
            for (std::size_t row = 0; row < output.capturedSwitches; ++row) {
                std::memcpy(&output.switches[row],
                            bytes + kSwitchRowsOffset + row * kSwitchRowStride,
                            sizeof output.switches[row]);
            }
        }
        if (output.progressCount > 0) {
            output.capturedProgressions = std::min<std::size_t>(
                static_cast<std::size_t>(output.progressCount), output.progressions.size());
            for (std::size_t row = 0; row < output.capturedProgressions; ++row) {
                std::memcpy(&output.progressions[row],
                            bytes + kProgressRowsOffset + row * kProgressRowStride,
                            sizeof output.progressions[row]);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void write_identity_line(std::uintptr_t callerRva, const SourceSnapshot& snapshot) noexcept {
    if (g_reports.load(std::memory_order_relaxed) >= kReportCapacity) {
        return;
    }
    const std::uint32_t sequence = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > kReportCapacity) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_state_source stage=identity n=%u caller=+0x%llX "
                                      "vtable=+0x%llX head=%08X,%08X,%08X,%08X",
                                      sequence,
                                      static_cast<unsigned long long>(callerRva),
                                      static_cast<unsigned long long>(snapshot.vtableRva),
                                      snapshot.prefixWords[0],
                                      snapshot.prefixWords[1],
                                      snapshot.prefixWords[2],
                                      snapshot.prefixWords[3]);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

[[nodiscard]] std::uintptr_t caller_rva(const void* returnAddress) noexcept {
    const auto* const caller = static_cast<const std::byte*>(returnAddress);
    const auto* const image =
        static_cast<const std::byte*>(static_cast<const void*>(GetModuleHandleW(nullptr)));
    return image != nullptr && caller >= image ? static_cast<std::uintptr_t>(caller - image) : 0;
}

/** FNV-mixes one scalar into a compact diagnostic de-duplication key. */
[[nodiscard]] std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
    for (std::size_t byte = 0; byte < sizeof value; ++byte) {
        hash ^= value & 0xFFU;
        hash *= kPrime;
        value >>= 8U;
    }
    return hash;
}

/** Claims one diagnostic tuple so repeated activity-bank rebuilds remain bounded. */
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

void write_line(const char* format,
                const char* stage,
                std::uintptr_t callerRva,
                std::uint32_t first,
                std::uint32_t second,
                std::uint32_t third,
                std::uint32_t fourth) noexcept {
    if (g_reports.load(std::memory_order_relaxed) >= kReportCapacity) {
        return;
    }
    const std::uint32_t sequence = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > kReportCapacity) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      format,
                                      stage,
                                      sequence,
                                      static_cast<unsigned long long>(callerRva),
                                      first,
                                      second,
                                      third,
                                      fourth);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

void report(std::uintptr_t callerRva, bool enabled, const SourceSnapshot& snapshot) noexcept {
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    std::uint64_t identityKey = mix(kOffset, callerRva);
    identityKey = mix(identityKey, snapshot.vtableRva);
    for (const std::uint32_t word : snapshot.prefixWords) {
        identityKey = mix(identityKey, word);
    }
    if (claim(identityKey)) {
        write_identity_line(callerRva, snapshot);
    }
    std::uint64_t summaryKey = mix(kOffset, callerRva);
    summaryKey = mix(summaryKey, static_cast<std::uint32_t>(snapshot.switchCount));
    summaryKey = mix(summaryKey, static_cast<std::uint32_t>(snapshot.progressCount));
    summaryKey = mix(summaryKey, enabled ? 1U : 0U);
    if (claim(summaryKey)) {
        write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX switches=%u "
                   "progressions=%u enabled=%u truncated=%u",
                   "summary",
                   callerRva,
                   static_cast<std::uint32_t>(snapshot.switchCount),
                   static_cast<std::uint32_t>(snapshot.progressCount),
                   enabled ? 1U : 0U,
                   snapshot.capturedSwitches
                               < static_cast<std::size_t>(std::max(snapshot.switchCount, 0))
                           || snapshot.capturedProgressions
                                  < static_cast<std::size_t>(std::max(snapshot.progressCount, 0))
                       ? 1U
                       : 0U);
    }
    for (const SwitchRow& row : std::span(snapshot.switches).first(snapshot.capturedSwitches)) {
        std::uint64_t key = mix(kOffset, 1);
        key = mix(key, callerRva);
        key = mix(key, static_cast<std::uint16_t>(row.index));
        key = mix(key, row.value);
        key = mix(key, row.auxiliary);
        if (claim(key)) {
            write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX index=%u "
                       "value=%u auxiliary=%u enabled=%u",
                       "switch",
                       callerRva,
                       static_cast<std::uint16_t>(row.index),
                       row.value,
                       row.auxiliary,
                       enabled ? 1U : 0U);
        }
    }
    for (const ProgressRow& row :
         std::span(snapshot.progressions).first(snapshot.capturedProgressions)) {
        std::uint64_t key = mix(kOffset, 2);
        key = mix(key, callerRva);
        key = mix(key, static_cast<std::uint16_t>(row.index));
        key = mix(key, row.value);
        key = mix(key, row.auxiliary);
        if (claim(key)) {
            write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX index=%u "
                       "value=%u auxiliary=%u enabled=%u",
                       "progression",
                       callerRva,
                       static_cast<std::uint16_t>(row.index),
                       row.value,
                       row.auxiliary,
                       enabled ? 1U : 0U);
        }
    }
}

void report_delta(const char* stage,
                  std::uint64_t side,
                  std::uintptr_t callerRva,
                  const SourceSnapshot& snapshot) noexcept {
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    std::uint64_t summaryKey = mix(kOffset, 3);
    summaryKey = mix(summaryKey, side);
    summaryKey = mix(summaryKey, callerRva);
    summaryKey = mix(summaryKey, static_cast<std::uint32_t>(snapshot.switchCount));
    summaryKey = mix(summaryKey, static_cast<std::uint32_t>(snapshot.progressCount));
    for (const std::uint32_t word : snapshot.prefixWords) {
        summaryKey = mix(summaryKey, word);
    }
    if (claim(summaryKey)) {
        write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX switches=%u "
                   "progressions=%u head0=%08X head1=%08X",
                   stage,
                   callerRva,
                   static_cast<std::uint32_t>(snapshot.switchCount),
                   static_cast<std::uint32_t>(snapshot.progressCount),
                   snapshot.prefixWords[0],
                   snapshot.prefixWords[1]);
    }
    for (const SwitchRow& row : std::span(snapshot.switches).first(snapshot.capturedSwitches)) {
        std::uint64_t key = mix(kOffset, 4);
        key = mix(key, side);
        key = mix(key, static_cast<std::uint16_t>(row.index));
        key = mix(key, row.value);
        key = mix(key, row.auxiliary);
        if (claim(key)) {
            write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX index=%u "
                       "value=%u auxiliary=%u reserved=%u",
                       side == 0 ? "delta_old_switch" : "delta_new_switch",
                       callerRva,
                       static_cast<std::uint16_t>(row.index),
                       row.value,
                       row.auxiliary,
                       0);
        }
    }
    for (const ProgressRow& row :
         std::span(snapshot.progressions).first(snapshot.capturedProgressions)) {
        std::uint64_t key = mix(kOffset, 5);
        key = mix(key, side);
        key = mix(key, static_cast<std::uint16_t>(row.index));
        key = mix(key, row.value);
        key = mix(key, row.auxiliary);
        if (claim(key)) {
            write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX index=%u "
                       "value=%u auxiliary=%u reserved=%u",
                       side == 0 ? "delta_old_progression" : "delta_new_progression",
                       callerRva,
                       static_cast<std::uint16_t>(row.index),
                       row.value,
                       row.auxiliary,
                       0);
        }
    }
}

/** Mirrors the category dispatcher and observes only its explicit-row category zero. */
__declspec(noinline) void __fastcall apply_category(void* first,
                                                    void* second,
                                                    void* third,
                                                    const void* source,
                                                    void* fifth,
                                                    void* sixth,
                                                    void* seventh,
                                                    void* eighth,
                                                    void* ninth,
                                                    std::int32_t category,
                                                    std::uintptr_t enabled,
                                                    void* twelfth,
                                                    void* destination,
                                                    void* status) noexcept {
    SourceSnapshot snapshot{};
    const bool captured = category == 0 && snapshot_source(source, snapshot);
    const std::uintptr_t callerRva = captured ? caller_rva(_ReturnAddress()) : 0;
    const ApplyCategory original = g_original.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(first,
                 second,
                 third,
                 source,
                 fifth,
                 sixth,
                 seventh,
                 eighth,
                 ninth,
                 category,
                 enabled,
                 twelfth,
                 destination,
                 status);
    }
    if (captured) {
        report(callerRva, (enabled & 0xFFU) != 0, snapshot);
    }
}

/** Mirrors the post-decode comparison and snapshots both owned state arguments before dispatch. */
__declspec(noinline) void __fastcall compare_source(void* oldSource,
                                                    const void* newSource) noexcept {
    SourceSnapshot oldSnapshot{};
    SourceSnapshot newSnapshot{};
    const bool capturedOld = snapshot_source(oldSource, oldSnapshot);
    const bool capturedNew = snapshot_source(newSource, newSnapshot);
    const std::uintptr_t callerRva = caller_rva(_ReturnAddress());
    const CompareSource original = g_originalCompare.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(oldSource, newSource);
    }
    if (capturedOld) {
        report_delta("delta_old", 0, callerRva, oldSnapshot);
    }
    if (capturedNew) {
        report_delta("delta_new", 1, callerRva, newSnapshot);
    }
}

void log_install(const char* result, const char* reason = nullptr) noexcept {
    std::array<char, 144> line{};
    const int written = reason == nullptr
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=activity_state_source stage=install result=%s "
                                            "dispatcher=+0x540320 comparator=+0xE07BA0",
                                            result)
                            : std::snprintf(line.data(),
                                            line.size(),
                                            "ev=activity_state_source stage=install result=%s "
                                            "reason=%s",
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

/** Attaches the read-only observer for explicit retained activity-state source rows. */
bool install_activity_state_source_observer() noexcept {
    if (g_applyHandle.attached && g_compareHandle.attached) {
        return true;
    }
    std::byte* const applyTarget =
        scan_main_image_unique(kApplySignature, "activity_state_category_dispatcher");
    std::byte* const compareTarget =
        scan_main_image_unique(kCompareSignature, "activity_state_source_comparator");
    if (applyTarget == nullptr || compareTarget == nullptr) {
        log_install("fail", applyTarget == nullptr ? "apply_target" : "compare_target");
        return false;
    }
    if (!hooking::detour::install({applyTarget, reinterpret_cast<void*>(&apply_category)},
                                  g_applyHandle)) {
        log_install("fail", "apply_attach");
        return false;
    }
    g_original.store(reinterpret_cast<ApplyCategory>(g_applyHandle.original),
                     std::memory_order_release);
    if (!hooking::detour::install({compareTarget, reinterpret_cast<void*>(&compare_source)},
                                  g_compareHandle)) {
        (void)hooking::detour::uninstall(g_applyHandle);
        g_original.store(nullptr, std::memory_order_release);
        log_install("fail", "compare_attach");
        return false;
    }
    g_originalCompare.store(reinterpret_cast<CompareSource>(g_compareHandle.original),
                            std::memory_order_release);
    log_install("ok");
    return true;
}

/** Detaches the activity-state source observer and clears its bounded diagnostics. */
void uninstall_activity_state_source_observer() noexcept {
    if (g_compareHandle.attached) {
        (void)hooking::detour::uninstall(g_compareHandle);
    }
    if (g_applyHandle.attached) {
        (void)hooking::detour::uninstall(g_applyHandle);
    }
    g_originalCompare.store(nullptr, std::memory_order_release);
    g_original.store(nullptr, std::memory_order_release);
    g_reports.store(0, std::memory_order_release);
    for (auto& entry : g_seen) {
        entry.store(0, std::memory_order_release);
    }
}

} // namespace sunrise::client::hooks::bootflow
