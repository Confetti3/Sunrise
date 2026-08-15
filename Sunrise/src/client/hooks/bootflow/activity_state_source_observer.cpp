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
#include "../../../state/activity/runtime.h"
#include "../../../state/activity/switch_commands/runtime.h"
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

/** Build-86657 definition-backed gameplay-switch writer. */
constexpr std::string_view kWriterSignatureText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 "
    "EC 20 41 8B E8 48 0F BF DA 48 8B F1 E8 ? ? ? ?";
constexpr auto kWriterSignature =
    signature<signature_length(kWriterSignatureText)>(kWriterSignatureText);

/** Build-86657 scenario-state list applier that owns a valid writer owner on its calling thread. */
constexpr std::string_view kApplyListsSignatureText =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 "
    "EC 20 33 F6 4D 8B F0 48 8B EA 48 8B D9 8B FE 48 39 32 7E 29";
constexpr auto kApplyListsSignature =
    signature<signature_length(kApplyListsSignatureText)>(kApplyListsSignatureText);

/** Build-86657 concrete activity-owner virtual method +0x90 at RVA +0xFA6CD0. */
constexpr std::string_view kOwnerUpdateSignatureText =
    "41 54 48 83 EC 40 48 8B 01 4C 8B E1 FF 90 88 00 00 00 84 C0 0F 85 ? ? ? ?";
constexpr auto kOwnerUpdateSignature =
    signature<signature_length(kOwnerUpdateSignatureText)>(kOwnerUpdateSignatureText);

/** Offset of the writer's near call to the definition-registry singleton getter. */
constexpr std::size_t kRegistryCallOffset = 36;
constexpr std::size_t kNearCallOperandOffset = 1;
constexpr std::size_t kNearCallLength = 5;
constexpr std::byte kNearCallOpcode{0xE8};

constexpr std::size_t kSwitchCountOffset = 0xB3F4;
constexpr std::size_t kSwitchRowsOffset = 0xB3F8;
constexpr std::size_t kSwitchRowStride = 4;
constexpr std::size_t kSwitchCapacity = 20;
constexpr std::size_t kProgressCountOffset = 0xB448;
constexpr std::size_t kProgressRowsOffset = 0xB44C;
constexpr std::size_t kProgressRowStride = 8;
constexpr std::size_t kProgressCaptureCapacity = 32;
constexpr std::size_t kPersistentSwitchBankOffset = 0x9348;
constexpr std::size_t kPersistentSwitchBankSize = 0x1000;
constexpr std::uint32_t kPersistentSourceIdentity = 0x00100101U;
constexpr std::uint16_t kArcadeHomecomingCandidateDefinition = 0x00F6;
constexpr std::size_t kArcadeHomecomingCandidateBankIndex = 1;
constexpr std::uintptr_t kActivityOwnerVtableRva = 0x1C35DC8;
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
using WriteSwitch = void(__fastcall*)(void*, std::uint16_t, std::int32_t) noexcept;
using ApplyLists = void(__fastcall*)(void*, const void*, const void*) noexcept;
using OwnerUpdate = void(__fastcall*)(void*) noexcept;
using StateGetter = void*(__fastcall*)(void*) noexcept;
using DefinitionRegistry = void*(__fastcall*)() noexcept;
using DefinitionTable = std::uintptr_t(__fastcall*)(void*) noexcept;

hooking::detour::Handle g_applyHandle{};
hooking::detour::Handle g_writerHandle{};
hooking::detour::Handle g_applyListsHandle{};
hooking::detour::Handle g_ownerUpdateHandle{};
std::atomic<ApplyCategory> g_original{nullptr};
std::atomic<WriteSwitch> g_originalWriter{nullptr};
std::atomic<ApplyLists> g_originalApplyLists{nullptr};
std::atomic<OwnerUpdate> g_originalOwnerUpdate{nullptr};
std::atomic<DefinitionRegistry> g_definitionRegistry{nullptr};
std::array<std::atomic<std::uint64_t>, kSeenCapacity> g_seen{};
std::atomic<std::uint32_t> g_reports{};
std::atomic<bool> g_definitionCandidatesReported{false};

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

struct SwitchListSnapshot {
    std::uintptr_t ownerVtableRva{};
    std::int32_t count{};
    std::size_t captured{};
    std::array<SwitchRow, kSwitchCapacity> rows{};
};

struct DefinitionRecordSnapshot {
    std::uint32_t prefix{};
    std::uint8_t type{};
    std::uint8_t flags{};
    std::int16_t bankIndex{};
};
static_assert(sizeof(DefinitionRecordSnapshot) == 8);

struct SourceSnapshot {
    std::array<std::uint32_t, 4> prefixWords{};
    std::uintptr_t vtableRva{};
    state::activity::WorldPhase worldPhase{state::activity::WorldPhase::idle};
    bool candidateStateValid{};
    std::uint8_t candidateState{};
    std::int32_t switchCount{};
    std::int32_t progressCount{};
    std::size_t capturedSwitches{};
    std::size_t capturedProgressions{};
    std::array<SwitchRow, kSwitchCapacity> switches{};
    std::array<ProgressRow, kProgressCaptureCapacity> progressions{};
};

struct OwnerUpdateSnapshot {
    void* owner{};
    void* source{};
    std::uintptr_t callerRva{};
    std::uintptr_t vtableRva{};
    state::activity::WorldPhase worldPhase{state::activity::WorldPhase::idle};
    std::uint8_t candidateState{};
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
        output.worldPhase = state::activity::world_phase();
        std::memcpy(output.prefixWords.data(), bytes, sizeof output.prefixWords);
        if (output.prefixWords[0] == kPersistentSourceIdentity) {
            std::memcpy(&output.candidateState,
                        bytes + kPersistentSwitchBankOffset
                            + kArcadeHomecomingCandidateBankIndex,
                        sizeof output.candidateState);
            output.candidateStateValid = true;
        }
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

/** Copies the transient scalar list consumed directly by the definition-backed writer. */
[[nodiscard]] bool snapshot_switch_list(void* owner,
                                        const void* list,
                                        SwitchListSnapshot& output) noexcept {
    if (owner == nullptr || list == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(list);
        const auto possibleVtable = reinterpret_cast<std::uintptr_t>(*static_cast<void***>(owner));
        output.ownerVtableRva = main_image_rva(possibleVtable);
        std::memcpy(&output.count, bytes, sizeof output.count);
        if (output.count < 0 || output.count > 0x10000) {
            return false;
        }
        std::int64_t relative{};
        std::memcpy(&relative, bytes + 8, sizeof relative);
        const auto* const rows = bytes + 0x18 + relative;
        output.captured = std::min<std::size_t>(static_cast<std::size_t>(output.count),
                                                output.rows.size());
        for (std::size_t row = 0; row < output.captured; ++row) {
            std::memcpy(&output.rows[row], rows + row * sizeof(SwitchRow), sizeof(SwitchRow));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Copies one retained switch byte while the concrete owner is valid on its update call. */
[[nodiscard]] bool snapshot_owner_update(void* owner, OwnerUpdateSnapshot& output) noexcept {
    if (owner == nullptr) {
        return false;
    }
    __try {
        output.owner = owner;
        const auto ownerVtable = reinterpret_cast<std::uintptr_t>(*static_cast<void***>(owner));
        output.vtableRva = main_image_rva(ownerVtable);
        if (output.vtableRva != kActivityOwnerVtableRva) {
            return false;
        }
        const auto* const ownerBytes = static_cast<const std::byte*>(owner);
        const void* source{};
        std::memcpy(&source, ownerBytes + 0x18, sizeof source);
        if (source == nullptr) {
            return false;
        }
        output.source = const_cast<void*>(source);
        const auto* const sourceBytes = static_cast<const std::byte*>(source);
        std::uint32_t identity{};
        std::memcpy(&identity, sourceBytes, sizeof identity);
        if (identity != kPersistentSourceIdentity) {
            return false;
        }
        output.worldPhase = state::activity::world_phase();
        std::memcpy(&output.candidateState,
                    sourceBytes + kPersistentSwitchBankOffset
                        + kArcadeHomecomingCandidateBankIndex,
                    sizeof output.candidateState);
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
    if (snapshot.candidateStateValid) {
        std::uint64_t candidateKey = mix(kOffset, 6);
        candidateKey = mix(candidateKey, callerRva);
        candidateKey = mix(candidateKey, static_cast<std::uint8_t>(snapshot.worldPhase));
        candidateKey = mix(candidateKey, snapshot.candidateState);
        if (claim(candidateKey)) {
            write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX definition=%u "
                       "bank_index=%u value=%u phase=%u",
                       "definition_state",
                       callerRva,
                       kArcadeHomecomingCandidateDefinition,
                       static_cast<std::uint32_t>(kArcadeHomecomingCandidateBankIndex),
                       snapshot.candidateState,
                       static_cast<std::uint8_t>(snapshot.worldPhase));
        }
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

void report_switch_write(std::uintptr_t callerRva,
                         std::uint16_t definition,
                         std::int32_t requested,
                         std::uint16_t index,
                         std::uint8_t before,
                         std::uint8_t after,
                         std::uint32_t changes) noexcept {
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    std::uint64_t key = mix(kOffset, 3);
    key = mix(key, callerRva);
    key = mix(key, definition);
    key = mix(key, static_cast<std::uint32_t>(requested));
    key = mix(key, index);
    key = mix(key, before);
    key = mix(key, after);
    key = mix(key, changes);
    if (!claim(key) || g_reports.load(std::memory_order_relaxed) >= kReportCapacity) {
        return;
    }
    const std::uint32_t sequence = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > kReportCapacity) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_state_source stage=switch_write n=%u caller=+0x%llX definition=%u "
        "requested=%d index=%u before=%u after=%u changes=%u",
        sequence,
        static_cast<unsigned long long>(callerRva),
        static_cast<unsigned>(definition),
        requested,
        static_cast<unsigned>(index),
        static_cast<unsigned>(before),
        static_cast<unsigned>(after),
        changes);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Reports one transient native switch list without preserving its owner or storage pointers. */
void report_switch_list(std::uintptr_t callerRva, const SwitchListSnapshot& snapshot) noexcept {
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    std::uint64_t summaryKey = mix(kOffset, 4);
    summaryKey = mix(summaryKey, callerRva);
    summaryKey = mix(summaryKey, snapshot.ownerVtableRva);
    summaryKey = mix(summaryKey, static_cast<std::uint32_t>(snapshot.count));
    if (claim(summaryKey)) {
        write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX owner_vtable=+0x%X "
                   "switches=%u truncated=%u",
                   "apply_lists",
                   callerRva,
                   static_cast<std::uint32_t>(snapshot.ownerVtableRva),
                   static_cast<std::uint32_t>(snapshot.count),
                   snapshot.captured < static_cast<std::size_t>(snapshot.count) ? 1U : 0U,
                   0U);
    }
    for (const SwitchRow& row : std::span(snapshot.rows).first(snapshot.captured)) {
        std::uint64_t key = mix(kOffset, 5);
        key = mix(key, callerRva);
        key = mix(key, snapshot.ownerVtableRva);
        key = mix(key, static_cast<std::uint16_t>(row.index));
        key = mix(key, row.value);
        key = mix(key, row.auxiliary);
        if (claim(key)) {
            write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX definition=%u "
                       "value=%u auxiliary=%u owner_vtable=+0x%X",
                       "apply_lists_switch",
                       callerRva,
                       static_cast<std::uint16_t>(row.index),
                       row.value,
                       row.auxiliary,
                       static_cast<std::uint32_t>(snapshot.ownerVtableRva));
        }
    }
}

void report_owner_update(const OwnerUpdateSnapshot& snapshot) noexcept {
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    std::uint64_t key = mix(kOffset, 7);
    key = mix(key, snapshot.callerRva);
    key = mix(key, snapshot.vtableRva);
    key = mix(key, static_cast<std::uint8_t>(snapshot.worldPhase));
    key = mix(key, snapshot.candidateState);
    if (!claim(key)) {
        return;
    }
    write_line("ev=activity_state_source stage=%s n=%u caller=+0x%llX definition=%u "
               "bank_index=%u value=%u phase=%u",
               "owner_update",
               snapshot.callerRva,
               kArcadeHomecomingCandidateDefinition,
               static_cast<std::uint32_t>(kArcadeHomecomingCandidateBankIndex),
               snapshot.candidateState,
               static_cast<std::uint8_t>(snapshot.worldPhase));
}

void report_owner_command(const state::activity::switch_commands::Command& command,
                          std::uint8_t before,
                          std::uint8_t after,
                          bool applied) noexcept {
    const bool reading =
        command.operation == state::activity::switch_commands::Operation::read;
    write_line("ev=activity_state_source stage=%s n=%u command=%llu definition=%u "
               "value=%u before=%u after=%u",
               reading ? (applied ? "owner_read_applied" : "owner_read_rejected")
                       : (applied ? "owner_set_applied" : "owner_set_rejected"),
               static_cast<std::uintptr_t>(command.sequence),
               command.definition,
               static_cast<std::uint32_t>(command.value),
               before,
               after);
}

/** Executes one claimed switch command while the validated retained source is transiently live. */
void apply_owner_command(const OwnerUpdateSnapshot& snapshot) noexcept {
    if (snapshot.worldPhase != state::activity::WorldPhase::arrived) {
        return;
    }
    state::activity::switch_commands::Command command{};
    if (!state::activity::switch_commands::try_claim(command)) {
        return;
    }
    std::uint8_t after = snapshot.candidateState;
    bool applied = command.definition == kArcadeHomecomingCandidateDefinition
                   && snapshot.source != nullptr;
    if (applied && command.operation == state::activity::switch_commands::Operation::set) {
        applied = command.value >= 0 && command.value <= 2 && snapshot.owner != nullptr;
        const WriteSwitch writer = g_originalWriter.load(std::memory_order_acquire);
        if (applied && writer != nullptr) {
            __try {
                writer(snapshot.owner, command.definition, command.value);
                const auto* const sourceBytes = static_cast<const std::byte*>(snapshot.source);
                std::memcpy(&after,
                            sourceBytes + kPersistentSwitchBankOffset
                                + kArcadeHomecomingCandidateBankIndex,
                            sizeof after);
                applied = after == static_cast<std::uint8_t>(command.value);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                applied = false;
            }
        } else {
            applied = false;
        }
    }
    state::activity::switch_commands::complete(
        command, snapshot.candidateState, after, applied);
    report_owner_command(command, snapshot.candidateState, after, applied);
}

/** Resolves one record through the same unpatched base table used by the native writer. */
[[nodiscard]] bool snapshot_definition_record(std::uint16_t definition,
                                              DefinitionRecordSnapshot& output) noexcept {
    const DefinitionRegistry registryGetter =
        g_definitionRegistry.load(std::memory_order_acquire);
    if (registryGetter == nullptr) {
        return false;
    }
    __try {
        void* const registry = registryGetter();
        if (registry == nullptr) {
            return false;
        }
        auto** const vtable = *static_cast<void***>(registry);
        const auto tableGetter =
            reinterpret_cast<DefinitionTable>(vtable[0x3C0 / sizeof(void*)]);
        if (tableGetter == nullptr) {
            return false;
        }
        const std::uintptr_t tableAddress = tableGetter(registry);
        if (tableAddress == 0) {
            return false;
        }
        const auto* const table = reinterpret_cast<const std::byte*>(tableAddress);
        std::int64_t relative{};
        std::memcpy(&relative, table + 8, sizeof relative);
        const auto* const record = table + 8 + relative
                                   + (static_cast<std::size_t>(definition) + 2U) * 8U;
        std::memcpy(&output, record, sizeof output);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Classifies the package-correlated Homecoming candidates once on an existing game-thread call. */
void report_definition_candidates() noexcept {
    if (g_definitionCandidatesReported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    constexpr std::array<std::uint16_t, 11> kCandidates{
        0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x0193, 0x019D,
        0x019E, 0x019F, 0x01A0, 0x01A1, 0x01A2};
    for (const std::uint16_t definition : kCandidates) {
        DefinitionRecordSnapshot snapshot{};
        const bool captured = snapshot_definition_record(definition, snapshot);
        std::array<char, kLineCapacity> line{};
        const int written = captured
                                ? std::snprintf(
                                      line.data(),
                                      line.size(),
                                      "ev=activity_state_source stage=definition_candidate "
                                      "definition=%u result=ok prefix=%08X type=%u flags=%u "
                                      "bank_index=%d source=base_table",
                                      static_cast<unsigned>(definition),
                                      snapshot.prefix,
                                      static_cast<unsigned>(snapshot.type),
                                      static_cast<unsigned>(snapshot.flags),
                                      static_cast<int>(snapshot.bankIndex))
                                : std::snprintf(
                                      line.data(),
                                      line.size(),
                                      "ev=activity_state_source stage=definition_candidate "
                                      "definition=%u result=unavailable source=base_table",
                                      static_cast<unsigned>(definition));
        if (written > 0) {
            const auto length = static_cast<std::size_t>(written) < line.size()
                                    ? static_cast<std::size_t>(written)
                                    : line.size() - 1;
            core::log::write(
                core::log::Channel::client, core::log::Level::info, {line.data(), length});
        }
    }
}

/** Obtains the index-1 retained state through the same owner getter the native writer uses. */
[[nodiscard]] void* persistent_state(void* owner) noexcept {
    if (owner == nullptr) {
        return nullptr;
    }
    __try {
        auto** const vtable = *static_cast<void***>(owner);
        const auto getter = reinterpret_cast<StateGetter>(vtable[0xA8 / sizeof(void*)]);
        return getter != nullptr ? getter(owner) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/** Copies the persistent 4 KiB switch bank only when the state identity is index 1. */
[[nodiscard]] bool snapshot_persistent_switches(
    void* owner, std::array<std::uint8_t, kPersistentSwitchBankSize>& output) noexcept {
    void* const source = persistent_state(owner);
    if (source == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(source);
        std::uint32_t identity{};
        std::memcpy(&identity, bytes, sizeof identity);
        if (identity != kPersistentSourceIdentity) {
            return false;
        }
        std::memcpy(output.data(), bytes + kPersistentSwitchBankOffset, output.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
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
        report_definition_candidates();
    }
}

/** Mirrors the definition-backed writer and reports any resulting index-1 bank delta. */
__declspec(noinline) void __fastcall write_switch(void* owner,
                                                  std::uint16_t definition,
                                                  std::int32_t requested) noexcept {
    std::array<std::uint8_t, kPersistentSwitchBankSize> before{};
    const bool capturedBefore = snapshot_persistent_switches(owner, before);
    const std::uintptr_t callerRva = caller_rva(_ReturnAddress());
    const WriteSwitch original = g_originalWriter.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(owner, definition, requested);
    }
    std::array<std::uint8_t, kPersistentSwitchBankSize> after{};
    if (!capturedBefore || !snapshot_persistent_switches(owner, after)) {
        return;
    }
    std::uint32_t changes = 0;
    std::uint16_t changedIndex = UINT16_MAX;
    std::uint8_t oldValue = 0;
    std::uint8_t newValue = 0;
    for (std::size_t index = 0; index < before.size(); ++index) {
        if (before[index] == after[index]) {
            continue;
        }
        if (changes == 0) {
            changedIndex = static_cast<std::uint16_t>(index);
            oldValue = before[index];
            newValue = after[index];
        }
        ++changes;
    }
    report_switch_write(
        callerRva, definition, requested, changedIndex, oldValue, newValue, changes);
}

/** Mirrors the native list applier and observes its pointer-free scalar input before consumption. */
__declspec(noinline) void __fastcall apply_lists(void* owner,
                                                 const void* switchList,
                                                 const void* progressionList) noexcept {
    SwitchListSnapshot snapshot{};
    const bool captured = snapshot_switch_list(owner, switchList, snapshot);
    const std::uintptr_t callerRva = captured ? caller_rva(_ReturnAddress()) : 0;
    if (captured) {
        report_switch_list(callerRva, snapshot);
    }
    const ApplyLists original = g_originalApplyLists.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(owner, switchList, progressionList);
    }
}

/** Observes one concrete owner update without retaining the owner or its state pointer. */
__declspec(noinline) void __fastcall update_owner(void* owner) noexcept {
    OwnerUpdateSnapshot snapshot{};
    const bool captured = snapshot_owner_update(owner, snapshot);
    if (captured) {
        snapshot.callerRva = caller_rva(_ReturnAddress());
    }
    const OwnerUpdate original = g_originalOwnerUpdate.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(owner);
    }
    if (captured) {
        apply_owner_command(snapshot);
        report_owner_update(snapshot);
    }
}

void log_install(const char* result, const char* reason = nullptr) noexcept {
    std::array<char, 144> line{};
    const int written = reason == nullptr
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=activity_state_source stage=install result=%s "
                                            "dispatcher=+0x540320 apply_lists=+0x52FD60 "
                                            "writer=+0x555EC0 owner_update=+0xFA6CD0",
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
    if (g_applyHandle.attached && g_applyListsHandle.attached && g_writerHandle.attached
        && g_ownerUpdateHandle.attached) {
        return true;
    }
    std::byte* const applyTarget =
        scan_main_image_unique(kApplySignature, "activity_state_category_dispatcher");
    std::byte* const writerTarget =
        scan_main_image_unique(kWriterSignature, "activity_state_switch_writer");
    std::byte* const applyListsTarget =
        scan_main_image_unique(kApplyListsSignature, "activity_state_apply_lists");
    std::byte* const ownerUpdateTarget =
        scan_main_image_unique(kOwnerUpdateSignature, "activity_owner_update");
    if (applyTarget == nullptr || applyListsTarget == nullptr || writerTarget == nullptr
        || ownerUpdateTarget == nullptr) {
        log_install("fail",
                    applyTarget == nullptr        ? "apply_target"
                    : applyListsTarget == nullptr ? "apply_lists_target"
                    : writerTarget == nullptr     ? "writer_target"
                                                  : "owner_update_target");
        return false;
    }
    const std::byte* const registryCall = writerTarget + kRegistryCallOffset;
    if (*registryCall != kNearCallOpcode) {
        log_install("fail", "registry_call_opcode");
        return false;
    }
    std::byte* const registryTarget =
        resolve_relative(registryCall + kNearCallOperandOffset,
                         registryCall + kNearCallLength);
    g_definitionRegistry.store(reinterpret_cast<DefinitionRegistry>(registryTarget),
                               std::memory_order_release);
    if (!hooking::detour::install({applyTarget, reinterpret_cast<void*>(&apply_category)},
                                  g_applyHandle)) {
        log_install("fail", "apply_attach");
        g_definitionRegistry.store(nullptr, std::memory_order_release);
        return false;
    }
    g_original.store(reinterpret_cast<ApplyCategory>(g_applyHandle.original),
                     std::memory_order_release);
    if (!hooking::detour::install({applyListsTarget, reinterpret_cast<void*>(&apply_lists)},
                                  g_applyListsHandle)) {
        (void)hooking::detour::uninstall(g_applyHandle);
        g_original.store(nullptr, std::memory_order_release);
        g_definitionRegistry.store(nullptr, std::memory_order_release);
        log_install("fail", "apply_lists_attach");
        return false;
    }
    g_originalApplyLists.store(reinterpret_cast<ApplyLists>(g_applyListsHandle.original),
                               std::memory_order_release);
    if (!hooking::detour::install({writerTarget, reinterpret_cast<void*>(&write_switch)},
                                  g_writerHandle)) {
        (void)hooking::detour::uninstall(g_applyListsHandle);
        (void)hooking::detour::uninstall(g_applyHandle);
        g_originalApplyLists.store(nullptr, std::memory_order_release);
        g_original.store(nullptr, std::memory_order_release);
        g_definitionRegistry.store(nullptr, std::memory_order_release);
        log_install("fail", "writer_attach");
        return false;
    }
    g_originalWriter.store(reinterpret_cast<WriteSwitch>(g_writerHandle.original),
                           std::memory_order_release);
    if (!hooking::detour::install({ownerUpdateTarget, reinterpret_cast<void*>(&update_owner)},
                                  g_ownerUpdateHandle)) {
        (void)hooking::detour::uninstall(g_writerHandle);
        (void)hooking::detour::uninstall(g_applyListsHandle);
        (void)hooking::detour::uninstall(g_applyHandle);
        g_originalWriter.store(nullptr, std::memory_order_release);
        g_originalApplyLists.store(nullptr, std::memory_order_release);
        g_original.store(nullptr, std::memory_order_release);
        g_definitionRegistry.store(nullptr, std::memory_order_release);
        log_install("fail", "owner_update_attach");
        return false;
    }
    g_originalOwnerUpdate.store(reinterpret_cast<OwnerUpdate>(g_ownerUpdateHandle.original),
                                std::memory_order_release);
    log_install("ok");
    return true;
}

/** Detaches the activity-state source observer and clears its bounded diagnostics. */
void uninstall_activity_state_source_observer() noexcept {
    if (g_ownerUpdateHandle.attached) {
        (void)hooking::detour::uninstall(g_ownerUpdateHandle);
    }
    if (g_writerHandle.attached) {
        (void)hooking::detour::uninstall(g_writerHandle);
    }
    if (g_applyListsHandle.attached) {
        (void)hooking::detour::uninstall(g_applyListsHandle);
    }
    if (g_applyHandle.attached) {
        (void)hooking::detour::uninstall(g_applyHandle);
    }
    g_originalWriter.store(nullptr, std::memory_order_release);
    g_originalOwnerUpdate.store(nullptr, std::memory_order_release);
    g_originalApplyLists.store(nullptr, std::memory_order_release);
    g_original.store(nullptr, std::memory_order_release);
    g_definitionRegistry.store(nullptr, std::memory_order_release);
    g_reports.store(0, std::memory_order_release);
    g_definitionCandidatesReported.store(false, std::memory_order_release);
    for (auto& entry : g_seen) {
        entry.store(0, std::memory_order_release);
    }
}

} // namespace sunrise::client::hooks::bootflow
