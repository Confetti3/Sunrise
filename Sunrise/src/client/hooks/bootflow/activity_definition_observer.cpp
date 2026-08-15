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

/** Build-86657 activity-index lookup whose result supplies a 0x28-byte definition record. */
constexpr std::string_view kLookupSignatureText =
    "48 89 5C 24 08 57 48 83 EC 20 0F B7 F9 33 DB E8 ? ? ? ? 84 C0 74 46 E8 ? ? ? ? "
    "0F B7 D7 48 8B C8 4C 8B 00 41 FF 90 60 04 00 00 48 8B C8 48 85 C0 74 29 48 8B 40 68";
/** Compiled lookup signature. */
constexpr auto kLookupSignature =
    signature<signature_length(kLookupSignatureText)>(kLookupSignatureText);

/** Return RVA immediately after the descriptor normalizer calls this lookup. */
constexpr std::uintptr_t kNormalizerReturnRva = 0xBFE0C5;
/** The normalizer copies exactly this many bytes from the returned definition. */
constexpr std::size_t kDefinitionSize = 0x28;
/** Fixed dword count covering the whole definition record. */
constexpr std::size_t kDefinitionDwords = kDefinitionSize / sizeof(std::uint32_t);
/** A complete launch should fit comfortably while a corrupt loop cannot fill the log. */
constexpr std::uint32_t kReportLimit = 128;
/** The 12-bit biased activity-selection wire field cannot carry an index above this. */
constexpr std::uint32_t kMaximumActivityIndex = 4'094;
/** All-one remains the unresolved local sentinel. */
constexpr std::uint16_t kUnresolvedActivityIndex = 0xFFFF;
/** Fixed event storage. */
constexpr std::size_t kLineCapacity = 288;

using Lookup = const void*(__fastcall*)(std::uint16_t) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<Lookup> g_original{nullptr};
std::atomic<std::uint32_t> g_reports{};
std::atomic_bool g_scanStarted{false};
std::atomic<std::uint16_t> g_homecomingIndex{kUnresolvedActivityIndex};

struct NamedIndex {
    std::string_view name;
    std::uint16_t index{};
    std::uint32_t matches{};
};

/** The two Homecoming graph variants and two independently observed controls. */
constexpr std::array<std::string_view, 4> kKnownNames{
    "arcade_homecoming",
    "mission_towerfall",
    "city_tower_social_d2",
    "mission_ember",
};

/** Copies the complete definition record without retaining its native pointer. */
[[nodiscard]] bool read_definition(
    const void* source, std::array<std::uint32_t, kDefinitionDwords>& output) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        std::memcpy(output.data(), source, kDefinitionSize);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Compares one native string including its terminator without retaining its pointer. */
[[nodiscard]] bool matches_name(const void* source, std::string_view expected) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        const auto* const text = static_cast<const char*>(source);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (text[index] != expected[index]) {
                return false;
            }
        }
        return text[expected.size()] == '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Emits one resolved name/index pair. */
void report_resolution(std::string_view name, std::uint16_t activityIndex) noexcept {
    std::array<char, 144> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_definition stage=resolve name=%.*s index=%u",
                                      static_cast<int>(name.size()),
                                      name.data(),
                                      static_cast<unsigned>(activityIndex));
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(
            core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Exhausts the real wire namespace once through the native read-only lookup. */
void scan_known_names(Lookup original) noexcept {
    std::array<NamedIndex, kKnownNames.size()> targets{};
    for (std::size_t index = 0; index < targets.size(); ++index) {
        targets[index].name = kKnownNames[index];
    }
    std::uint32_t searched = 0;
    for (std::uint32_t index = 0; index <= kMaximumActivityIndex; ++index) {
        ++searched;
        const void* const candidate = original(static_cast<std::uint16_t>(index));
        for (NamedIndex& target : targets) {
            if (matches_name(candidate, target.name)) {
                if (target.matches == 0) {
                    target.index = static_cast<std::uint16_t>(index);
                }
                ++target.matches;
                report_resolution(target.name, static_cast<std::uint16_t>(index));
            }
        }
    }
    if (targets[1].matches != 0) {
        g_homecomingIndex.store(targets[1].index, std::memory_order_release);
    }
    std::array<char, 184> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_definition stage=scan result=%s searched=%u arcade=%d/%u towerfall=%d/%u "
        "tower=%d/%u ember=%d/%u",
        targets[0].matches != 0 && targets[1].matches != 0 && targets[2].matches != 0
                && targets[3].matches != 0
            ? "complete"
            : "partial",
        searched,
        targets[0].matches != 0 ? static_cast<int>(targets[0].index) : -1,
        targets[0].matches,
        targets[1].matches != 0 ? static_cast<int>(targets[1].index) : -1,
        targets[1].matches,
        targets[2].matches != 0 ? static_cast<int>(targets[2].index) : -1,
        targets[2].matches,
        targets[3].matches != 0 ? static_cast<int>(targets[3].index) : -1,
        targets[3].matches);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(
            core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Emits one bounded definition observation from the proven normalizer call site. */
void report(std::uintptr_t returnRva,
            std::uint16_t activityIndex,
            const std::array<std::uint32_t, kDefinitionDwords>& words) noexcept {
    const std::uint32_t seen = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen > kReportLimit) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_definition stage=lookup n=%u caller=+0x%llX index=%u "
        "words=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X",
        seen,
        static_cast<unsigned long long>(returnRva),
        static_cast<unsigned>(activityIndex),
        words[0],
        words[1],
        words[2],
        words[3],
        words[4],
        words[5],
        words[6],
        words[7],
        words[8],
        words[9]);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(
            core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Mirrors the native lookup and observes only its descriptor-normalizer caller. */
__declspec(noinline) const void* __fastcall lookup(std::uint16_t activityIndex) noexcept {
    const auto* const caller = static_cast<const std::byte*>(_ReturnAddress());
    const auto* const image = static_cast<const std::byte*>(static_cast<const void*>(
        GetModuleHandleW(nullptr)));
    const std::uintptr_t returnRva = image != nullptr && caller >= image
                                         ? static_cast<std::uintptr_t>(caller - image)
                                         : 0;
    const Lookup original = g_original.load(std::memory_order_acquire);
    const void* const result = original != nullptr ? original(activityIndex) : nullptr;
    std::array<std::uint32_t, kDefinitionDwords> words{};
    if (returnRva == kNormalizerReturnRva && read_definition(result, words)) {
        report(returnRva, activityIndex, words);
        if (!g_scanStarted.exchange(true, std::memory_order_acq_rel) && original != nullptr) {
            scan_known_names(original);
        }
    }
    return result;
}

/** Writes a named failure without turning this diagnostic miss into a crash. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 120> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_definition stage=install result=fail reason=%s",
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

/** Attaches the read-only activity-definition lookup observer. */
bool install_activity_definition_observer() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target =
        scan_main_image_unique(kLookupSignature, "activity_definition_lookup");
    if (target == nullptr) {
        return fail("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&lookup)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    g_original.store(reinterpret_cast<Lookup>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=activity_definition stage=install result=ok");
    return true;
}

/** Copies the graph-resolved Homecoming index after the one-time native namespace scan. */
bool homecoming_activity_index(std::uint16_t& output) noexcept {
    const std::uint16_t index = g_homecomingIndex.load(std::memory_order_acquire);
    if (index == kUnresolvedActivityIndex) {
        return false;
    }
    output = index;
    return true;
}

/** Detaches the activity-definition lookup observer. */
void uninstall_activity_definition_observer() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_scanStarted.store(false, std::memory_order_release);
    g_homecomingIndex.store(kUnresolvedActivityIndex, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
