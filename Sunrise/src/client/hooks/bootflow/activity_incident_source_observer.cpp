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

/**
 * Build-86657 client incident source. It copies target ids and a 0x400-byte body into a bounded
 * 0x510-byte request before dispatching the retail `send_incident` operation.
 */
constexpr std::string_view kIncidentSourceSignatureText =
    "48 89 5C 24 18 57 48 81 EC 60 05 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 50 05 "
    "00 00 48 8B DA 48 8B F9 E8 ? ? ? ? F6 00 02";
/** Compiled source signature. */
constexpr auto kIncidentSourceSignature =
    signature<signature_length(kIncidentSourceSignatureText)>(kIncidentSourceSignatureText);

/** The retail request allocates room for this many target ids. */
constexpr std::uint32_t kTargetCapacity = 67;
/** Number of target ids retained in one diagnostic event. */
constexpr std::size_t kLoggedTargetCapacity = 8;
/** Offset of the first target id in the native source row. */
constexpr std::size_t kFirstTargetOffset = 0x0C;
/** Native source rows reserve 16 bytes per target. */
constexpr std::size_t kTargetStride = 0x10;
/** Offset of the pointer to the fixed incident body. */
constexpr std::size_t kBodyPointerOffset = 0x420;
/** Bytes copied by the native incident source. */
constexpr std::size_t kBodySize = 0x400;
/** Body prefix retained as scalars for comparison with downstream observations. */
constexpr std::size_t kBodyWordCapacity = 4;
/** A complete session should fit while a corrupt loop cannot fill the log. */
constexpr std::uint32_t kReportLimit = 128;
/** Fixed diagnostic storage. */
constexpr std::size_t kLineCapacity = 384;

using IncidentSource = std::uint64_t(__fastcall*)(void*, const void*) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<IncidentSource> g_original{nullptr};
std::atomic<std::uint32_t> g_reports{};

struct Snapshot {
    std::uint32_t targetCount{};
    std::array<std::uint32_t, kLoggedTargetCapacity> targets{};
    std::array<std::uint32_t, kBodyWordCapacity> bodyWords{};
    std::uint64_t bodyHash{};
};

/** Hashes the fixed copied body so separate stages can be correlated without dumping it. */
[[nodiscard]] std::uint64_t hash_body(const std::byte* body) noexcept {
    constexpr std::uint64_t kOffsetBasis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
    std::uint64_t hash = kOffsetBasis;
    for (std::size_t index = 0; index < kBodySize; ++index) {
        hash ^= std::to_integer<std::uint8_t>(body[index]);
        hash *= kPrime;
    }
    return hash;
}

/** Copies only bounded, pointer-free incident source data during the call that owns it. */
[[nodiscard]] bool snapshot(const void* source, Snapshot& output) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(source);
        std::memcpy(&output.targetCount, bytes, sizeof output.targetCount);
        if (output.targetCount > kTargetCapacity) {
            return false;
        }
        const std::size_t retained =
            output.targetCount < output.targets.size() ? output.targetCount : output.targets.size();
        for (std::size_t index = 0; index < retained; ++index) {
            std::memcpy(&output.targets[index],
                        bytes + kFirstTargetOffset + index * kTargetStride,
                        sizeof output.targets[index]);
        }
        const std::byte* body = nullptr;
        std::memcpy(&body, bytes + kBodyPointerOffset, sizeof body);
        if (body == nullptr) {
            return false;
        }
        std::memcpy(output.bodyWords.data(), body, sizeof output.bodyWords);
        output.bodyHash = hash_body(body);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Emits one bounded incident-source observation. */
void report(std::uintptr_t callerRva, const Snapshot& value) noexcept {
    const std::uint32_t sequence = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > kReportLimit) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity_incident_source stage=send n=%u caller=+0x%llX count=%u "
                      "targets=%u,%u,%u,%u,%u,%u,%u,%u body_hash=%016llX words=%08X,%08X,%08X,%08X",
                      sequence,
                      static_cast<unsigned long long>(callerRva),
                      value.targetCount,
                      value.targets[0],
                      value.targets[1],
                      value.targets[2],
                      value.targets[3],
                      value.targets[4],
                      value.targets[5],
                      value.targets[6],
                      value.targets[7],
                      static_cast<unsigned long long>(value.bodyHash),
                      value.bodyWords[0],
                      value.bodyWords[1],
                      value.bodyWords[2],
                      value.bodyWords[3]);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Mirrors the native source and observes its input only for the duration of this call. */
__declspec(noinline) std::uint64_t __fastcall incident_source(void* owner,
                                                              const void* source) noexcept {
    const auto* const caller = static_cast<const std::byte*>(_ReturnAddress());
    const auto* const image =
        static_cast<const std::byte*>(static_cast<const void*>(GetModuleHandleW(nullptr)));
    const std::uintptr_t callerRva =
        image != nullptr && caller >= image ? static_cast<std::uintptr_t>(caller - image) : 0;
    Snapshot value{};
    if (snapshot(source, value)) {
        report(callerRva, value);
    }
    const IncidentSource original = g_original.load(std::memory_order_acquire);
    if (original != nullptr) {
        return original(owner, source);
    }
    return 0;
}

/** Writes a named failure without turning this optional probe into a boot failure. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_incident_source stage=install result=fail "
                                      "reason=%s",
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

/** Attaches the read-only build-86657 client incident-source observer. */
bool install_activity_incident_source_observer() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target =
        scan_main_image_unique(kIncidentSourceSignature, "activity_incident_source");
    if (target == nullptr) {
        return fail("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&incident_source)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    g_original.store(reinterpret_cast<IncidentSource>(g_handle.original),
                     std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=activity_incident_source stage=install result=ok rva=+0xD82730");
    return true;
}

/** Detaches the client incident-source observer. */
void uninstall_activity_incident_source_observer() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reports.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
