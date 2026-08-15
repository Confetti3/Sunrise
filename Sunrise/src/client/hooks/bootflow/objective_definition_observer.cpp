#include <Windows.h>

#include <algorithm>
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
constexpr std::size_t kExpressionNodeCapacity = 24;
constexpr std::uint32_t kReportLimit = 256;
constexpr std::size_t kLineCapacity = 512;
constexpr std::uint64_t kOccupiedBit = 0x8000'0000'0000'0000ULL;

struct OutputPair {
    const void* primary{};
    const void* secondary{};
};

struct ExpressionNode {
    std::uint8_t opcode{};
    std::array<std::uint8_t, 3> metadata{};
    std::uint32_t operand{};
};
static_assert(sizeof(ExpressionNode) == 8);

struct RecordSnapshot {
    bool present{};
    std::array<std::uint32_t, kRecordWords> prefix{};
    std::uint32_t maximum{};
    std::uint8_t flag28{};
    std::uint8_t flag2c{};
    std::int32_t expressionCount{};
    std::size_t capturedNodes{};
    std::array<ExpressionNode, kExpressionNodeCapacity> nodes{};
};

struct Snapshot {
    RecordSnapshot primary{};
    RecordSnapshot secondary{};
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

/** Copies one definition prefix and its bounded RPN expression while the call owns it. */
[[nodiscard]] bool snapshot_record(const void* record, RecordSnapshot& output) noexcept {
    if (record == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(record);
        output.present = true;
        std::memcpy(output.prefix.data(), bytes, sizeof output.prefix);
        std::memcpy(&output.maximum, bytes + 0x30, sizeof output.maximum);
        std::memcpy(&output.flag28, bytes + 0x28, sizeof output.flag28);
        std::memcpy(&output.flag2c, bytes + 0x2C, sizeof output.flag2c);
        const auto* const expression = bytes + 0x38;
        std::memcpy(&output.expressionCount, expression, sizeof output.expressionCount);
        if (output.expressionCount > 0
            && output.expressionCount <= static_cast<std::int32_t>(output.nodes.size())) {
            std::int64_t relative{};
            std::memcpy(&relative, expression + 8, sizeof relative);
            const auto* const nodes = expression + 0x18 + relative;
            output.capturedNodes = static_cast<std::size_t>(output.expressionCount);
            std::memcpy(output.nodes.data(), nodes, output.capturedNodes * sizeof(ExpressionNode));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Copies bounded, pointer-free state from each resolved definition record. */
[[nodiscard]] bool snapshot(const OutputPair* pair, Snapshot& output) noexcept {
    if (pair == nullptr) {
        return false;
    }
    __try {
        const OutputPair local = *pair;
        const bool primary = snapshot_record(local.primary, output.primary);
        const bool secondary = snapshot_record(local.secondary, output.secondary);
        return primary || secondary || (local.primary == nullptr && local.secondary == nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Emits the bounded expression nodes belonging to one resolved side. */
void report_expression(std::uint32_t sequence,
                       std::uintptr_t callerRva,
                       std::uint16_t objectiveIndex,
                       char side,
                       const RecordSnapshot& record) noexcept {
    for (std::size_t index = 0; index < record.capturedNodes; ++index) {
        const ExpressionNode& node = record.nodes[index];
        std::array<char, kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=objective_definition stage=expression n=%u caller=+0x%llX index=%u side=%c "
            "node=%u opcode=%u meta=%02X%02X%02X operand=%08X",
            sequence,
            static_cast<unsigned long long>(callerRva),
            static_cast<unsigned>(objectiveIndex),
            side,
            static_cast<unsigned>(index),
            static_cast<unsigned>(node.opcode),
            static_cast<unsigned>(node.metadata[0]),
            static_cast<unsigned>(node.metadata[1]),
            static_cast<unsigned>(node.metadata[2]),
            node.operand);
        if (written > 0) {
            const auto length = static_cast<std::size_t>(written) < line.size()
                                    ? static_cast<std::size_t>(written)
                                    : line.size() - 1;
            core::log::write(
                core::log::Channel::client, core::log::Level::info, {line.data(), length});
        }
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
        "primary=%08X,%08X,%08X,%08X secondary=%08X,%08X,%08X,%08X "
        "max=%u,%u flags=%u,%u,%u,%u expression=%d,%d captured=%u,%u",
        sequence,
        static_cast<unsigned long long>(callerRva),
        static_cast<unsigned>(objectiveIndex),
        value.primary.present ? 1U : 0U,
        value.secondary.present ? 1U : 0U,
        value.primary.prefix[0],
        value.primary.prefix[1],
        value.primary.prefix[2],
        value.primary.prefix[3],
        value.secondary.prefix[0],
        value.secondary.prefix[1],
        value.secondary.prefix[2],
        value.secondary.prefix[3],
        value.primary.maximum,
        value.secondary.maximum,
        static_cast<unsigned>(value.primary.flag28),
        static_cast<unsigned>(value.primary.flag2c),
        static_cast<unsigned>(value.secondary.flag28),
        static_cast<unsigned>(value.secondary.flag2c),
        value.primary.expressionCount,
        value.secondary.expressionCount,
        static_cast<unsigned>(value.primary.capturedNodes),
        static_cast<unsigned>(value.secondary.capturedNodes));
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
    report_expression(sequence, callerRva, objectiveIndex, 'p', value.primary);
    report_expression(sequence, callerRva, objectiveIndex, 's', value.secondary);
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
