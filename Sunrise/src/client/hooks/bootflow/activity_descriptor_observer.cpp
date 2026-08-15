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
 * Build-86657 descriptor assignment entry. The body compares or installs the 0x118-byte payload
 * held at destination + 0x148. The signature spans its register setup and state dispatch.
 */
constexpr std::string_view kAssignmentSignatureText =
    "48 89 5C 24 10 56 48 83 EC 20 48 8B DA 48 8B F1 48 8B 51 18 8B 8A F8 AE 01 00 8D 41 FC";
/** Compiled assignment signature. */
constexpr auto kAssignmentSignature =
    signature<signature_length(kAssignmentSignatureText)>(kAssignmentSignatureText);

/** Return RVAs immediately after calls that target controller + 0x8E08. */
constexpr std::array<std::uintptr_t, 9> kActivityReturnRvas{
    0xBF994C,
    0xBF9AA7,
    0xC1993A,
    0xC19DF6,
    0xC19EAC,
    0xD4BC16,
    0xE1C19C,
    0xEE8546,
    0xEEAFEF,
};
/** A complete launch should fit comfortably while a corrupt loop cannot fill the log. */
constexpr std::uint32_t kReportLimit = 128;
/** Fixed event storage. */
constexpr std::size_t kLineCapacity = 160;

using Assignment = bool(__fastcall*)(void*, const void*) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<Assignment> g_original{nullptr};
std::atomic<std::uint32_t> g_reports{};

struct DescriptorScalars {
    std::uint8_t type{};
    std::uint16_t primary{};
    std::uint16_t overrideValue{};
};

/** @return True when this return RVA is one of the proven controller descriptor writers. */
[[nodiscard]] bool is_activity_writer(std::uintptr_t returnRva) noexcept {
    for (const std::uintptr_t known : kActivityReturnRvas) {
        if (returnRva == known) {
            return true;
        }
    }
    return false;
}

/** Reads only the three descriptor fields already consumed at the known call sites. */
[[nodiscard]] bool read_scalars(const void* source, DescriptorScalars& output) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        const auto* const bytes = static_cast<const std::byte*>(source);
        output.type = std::to_integer<std::uint8_t>(bytes[0]);
        std::memcpy(&output.primary, bytes + 2, sizeof output.primary);
        std::memcpy(&output.overrideValue, bytes + 4, sizeof output.overrideValue);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Emits one bounded observation after the native assignment has completed. */
void report(std::uintptr_t returnRva, const DescriptorScalars& descriptor, bool result) noexcept {
    const std::uint32_t seen = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen > kReportLimit) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_descriptor stage=assign n=%u caller=+0x%llX "
                                      "type=%u primary=%u override=%u native_result=%u",
                                      seen,
                                      static_cast<unsigned long long>(returnRva),
                                      static_cast<unsigned>(descriptor.type),
                                      static_cast<unsigned>(descriptor.primary),
                                      static_cast<unsigned>(descriptor.overrideValue),
                                      result ? 1U : 0U);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(
            core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Mirrors the native assignment and observes only its nine controller call sites. */
__declspec(noinline) bool __fastcall assignment(void* destination, const void* source) noexcept {
    const auto* const caller = static_cast<const std::byte*>(_ReturnAddress());
    const auto* const image = static_cast<const std::byte*>(static_cast<const void*>(
        GetModuleHandleW(nullptr)));
    const std::uintptr_t returnRva = image != nullptr && caller >= image
                                         ? static_cast<std::uintptr_t>(caller - image)
                                         : 0;
    DescriptorScalars descriptor{};
    const bool observe = is_activity_writer(returnRva) && read_scalars(source, descriptor);
    const Assignment original = g_original.load(std::memory_order_acquire);
    const bool result = original != nullptr && original(destination, source);
    if (observe) {
        report(returnRva, descriptor, result);
    }
    return result;
}

/** Writes a named failure without turning this diagnostic miss into a crash. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 112> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_descriptor stage=install result=fail reason=%s",
                                      reason);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), length});
    }
    return false;
}

} // namespace

/** Attaches the read-only controller activity-descriptor observer. */
bool install_activity_descriptor_observer() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target =
        scan_main_image_unique(kAssignmentSignature, "activity_descriptor_assignment");
    if (target == nullptr) {
        return fail("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&assignment)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    g_original.store(reinterpret_cast<Assignment>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=activity_descriptor stage=install result=ok");
    return true;
}

/** Detaches the descriptor observer. */
void uninstall_activity_descriptor_observer() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
