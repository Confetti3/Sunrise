/** Independent first-person weapon and native game-HUD presentation policies. */

#include "presentation.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <limits>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"
#include "../../player/player_settings_store.h"
#include "../teleport/internal.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::presentation {
namespace {

/** Build-86657 settings records begin at +0x10; HUD opacity is item 20 at record +0x3C. */
constexpr std::size_t kSettingsRecordOffset = 0x10;
constexpr std::size_t kHudOpacityRecordOffset = 0x3C;
constexpr std::size_t kHudOpacityOffset = kSettingsRecordOffset + kHudOpacityRecordOffset;
/** Native HUD opacity choices span Off through Full, encoded as 0 through 3. */
constexpr std::uint8_t kHudOpacityOff = 0;
constexpr std::uint8_t kHudOpacityMaximum = 3;

/** Unique caller containing both native HUD settings calls used for structural validation. */
constexpr std::string_view kHudRowCallerText =
    "0F BE 8F 88 00 00 00 83 F9 03 0F 87 ? ? ? ? E8 ? ? ? ? "
    "48 85 C0 0F 84 ? ? ? ? 48 8D 50 10 48 85 D2 0F 84 ? ? ? ? "
    "41 0F B6 0E 84 C9 0F 84 ? ? ? ? 32 DB 44 0F B6 C1 88 5C "
    "24 40 41 8D 80 4E FF FF FF 83 F8 05 41 0F 96 C1 84 C9 74 ? "
    "41 8D 40 FF 83 F8 31 76 ? 45 84 C9 75 ? 41 8D 80 48 FF FF "
    "FF 83 F8 31 77 ? E8 ? ? ? ?";
/** Masked caller form consumed by the image scanner. */
constexpr auto kHudRowCaller =
    patterns::signature<patterns::signature_length(kHudRowCallerText)>(kHudRowCallerText);
/** Near-call offsets measured from the validated HUD caller. */
constexpr std::size_t kHudLookupCall = 0x10;
constexpr std::size_t kHudRowReaderCall = 0x6D;

/** Native lookup that returns one player's settings record. */
constexpr std::string_view kSettingsRecordLookupText =
    "48 83 EC 38 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 28 83 F9 FF 74 63 "
    "48 63 C1 48 69 C8 F0 12 00 00";
/** Masked settings-lookup form consumed by the image scanner. */
constexpr auto kSettingsRecordLookup =
    patterns::signature<patterns::signature_length(kSettingsRecordLookupText)>(
        kSettingsRecordLookupText);

/** Native row reader whose item-20 case reads HUD opacity. */
constexpr std::string_view kHudRowReaderText =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 0F B6 F1 "
    "48 8B FA 8B CE 83 CB FF E8 ? ? ? ? 85 C0 74 24 44 8B C0 "
    "48 8D 4C 24 30 48 8B D7 E8 ? ? ? ? 8B 18 8B C3";
/** Masked HUD row-reader form consumed by the image scanner. */
constexpr auto kHudRowReader =
    patterns::signature<patterns::signature_length(kHudRowReaderText)>(kHudRowReaderText);

/** Item-20 return path used to prove the row-reader layout and field offset together. */
constexpr std::string_view kHudItem20Text =
    "0F BE 5F 3C 8B C3 48 8B 5C 24 38 48 8B 74 24 40 48 83 C4 20 5F C3";
/** Masked item-20 form consumed by the image scanner. */
constexpr auto kHudItem20 =
    patterns::signature<patterns::signature_length(kHudItem20Text)>(kHudItem20Text);
/** Item-20 return path offset measured from the validated row-reader entry. */
constexpr std::size_t kHudItem20InRowReader = 0x2D2;

/** Unique first-person update caller used to derive the otherwise repeated rig transform. */
constexpr std::string_view kFirstPersonRigUpdateText =
    "48 89 5C 24 18 55 56 57 48 8D 6C 24 C0 48 81 EC 40 01 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 45 D0 48 8B F1 33 C9 E8 ? ? ? ? "
    "44 8B 86 10 01 00 00 85 C0 8B 5E 2C 40 0F 94 C7 41 83 F8 FF";
/** Masked rig-update form consumed by the image scanner. */
constexpr auto kFirstPersonRigUpdate =
    patterns::signature<patterns::signature_length(kFirstPersonRigUpdateText)>(
        kFirstPersonRigUpdateText);
/** Transform-submission call offset measured from the validated rig-update entry. */
constexpr std::size_t kFirstPersonRigTransformCall = 0x769;

/** Repeated rig-transform entry validated only at the target decoded from the unique caller. */
constexpr std::string_view kFirstPersonRigTransformText =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B 1D ? ? ? ? 48 8B FA 48 85 DB "
    "0F 84 ? ? ? ? 48 89 5C 24 38 E8 ? ? ? ? 33 C3 89 44 24 38 "
    "E8 ? ? ? ? 31 44 24 3C";
/** Masked rig-transform form used for validation at the decoded call target. */
constexpr auto kFirstPersonRigTransform =
    patterns::signature<patterns::signature_length(kFirstPersonRigTransformText)>(
        kFirstPersonRigTransformText);
/** RIP-relative global-load offset measured from the validated transform entry. */
constexpr std::size_t kFirstPersonRigGlobalLoad = 0x0A;
/** Proven finite render-only displacement; stock submission restores the animated pose. */
constexpr float kHiddenRigOffset = 1'000'000.0F;

/** Aligned transform passed by the first-person update path to its native submitter. */
struct alignas(16) FirstPersonRigTransform {
    std::array<float, 4> orientation{};
    std::array<float, 4> position{};
};
static_assert(sizeof(FirstPersonRigTransform) == 32);
static_assert(alignof(FirstPersonRigTransform) == 16);

using FirstPersonRigTransformSubmit = void(__fastcall*)(std::byte*, const FirstPersonRigTransform*);
using SettingsRecordLookup = std::byte*(__fastcall*)(std::uint32_t);
using HudRowReader = std::int32_t(__fastcall*)(std::uint8_t, const std::byte*);

/** HUD value ownership carried across frames; no settings-record pointer is retained. */
struct HudSession {
    std::uint32_t playerIndex{teleport::kInvalidHandle};
    std::uint8_t prior{};
    bool captured{};
    bool authored{};
    bool released{};
    bool suppressedReported{};
    bool faultReported{};
};

hooking::detour::Handle g_handle{};
std::atomic<FirstPersonRigTransformSubmit> g_originalRigTransform{nullptr};
std::atomic<SettingsRecordLookup> g_settingsRecordLookup{nullptr};
std::atomic<const void*> g_rigTransformReturn{nullptr};
std::atomic_bool g_installed{};
std::atomic_bool g_stopping{};
std::atomic_bool g_installPublishing{};
std::atomic_bool g_hideWeaponRequested{};
std::atomic_bool g_removeHudRequested{};
std::atomic_bool g_weaponHidden{};
std::atomic_bool g_hudHidden{};
std::atomic_bool g_hudFault{};
std::atomic_uint g_replacementInFlight{};
SRWLOCK g_hudLock{SRWLOCK_INIT};
HudSession g_hud{};

/** Reads one typed value through the current-process API without dereferencing untrusted memory. */
template <typename T> [[nodiscard]] bool read_at(const std::byte* address, T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/** Writes one typed value through the current-process API only when every byte is accepted. */
template <typename T> [[nodiscard]] bool write_at(std::byte* address, const T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &written) != FALSE
           && written == sizeof value;
}

/** @return True when an address belongs to committed, readable process memory. */
[[nodiscard]] bool committed_address(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    return address != nullptr
           && VirtualQuery(address, &information, sizeof information) == sizeof information
           && information.State == MEM_COMMIT
           && (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

/** @return True when an address belongs to one executable memory protection class. */
[[nodiscard]] bool executable_address(const void* address) noexcept {
    if (!committed_address(address)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION information{};
    (void)VirtualQuery(address, &information, sizeof information);
    const DWORD protection = information.Protect & 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
           || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

/** Decodes and validates one near-call target at a known offset. */
template <typename T> [[nodiscard]] T call_target(std::byte* base, std::size_t offset) noexcept {
    constexpr std::byte kNearCall{0xE8};
    std::byte opcode{};
    std::int32_t displacement{};
    std::byte* const instruction = base != nullptr ? base + offset : nullptr;
    if (!read_at(instruction, opcode) || opcode != kNearCall
        || !read_at(instruction + 1, displacement)) {
        return nullptr;
    }
    std::byte* const target = instruction + 5 + displacement;
    return executable_address(target) ? reinterpret_cast<T>(target) : nullptr;
}

/** Decodes a RIP-relative seven-byte load and validates the referenced memory. */
[[nodiscard]] std::byte* rip_target(std::byte* instruction) noexcept {
    std::int32_t displacement{};
    if (instruction == nullptr || !read_at(instruction + 3, displacement)) {
        return nullptr;
    }
    std::byte* const target = instruction + 7 + displacement;
    return committed_address(target) ? target : nullptr;
}

/** @return True when every lane can be submitted as a finite render transform. */
template <std::size_t Count>
[[nodiscard]] bool finite(const std::array<float, Count>& values) noexcept {
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

/** Compares one decoded target with a compiled masked signature in place. */
template <std::size_t Count>
[[nodiscard]] bool
matches_signature(const std::byte* address,
                  const std::array<patterns::PatternByte, Count>& signature) noexcept {
    std::array<std::byte, Count> bytes{};
    if (!read_at(address, bytes)) {
        return false;
    }
    for (std::size_t index = 0; index < Count; ++index) {
        if (signature[index].exact && signature[index].value != bytes[index]) {
            return false;
        }
    }
    return true;
}

/** Writes one bounded structured presentation event. */
void report(const char* stage, const char* result, const char* reason = nullptr) noexcept {
    std::array<char, 176> line{};
    const int written =
        reason == nullptr
            ? std::snprintf(
                  line.data(), line.size(), "ev=presentation stage=%s result=%s", stage, result)
            : std::snprintf(line.data(),
                            line.size(),
                            "ev=presentation stage=%s result=%s reason=%s",
                            stage,
                            result,
                            reason);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), length});
    }
}

/** Resolves the current player's HUD-opacity byte without retaining the returned record. */
[[nodiscard]] bool hud_opacity_address(std::uint32_t playerIndex, std::byte*& address) noexcept {
    address = nullptr;
    const SettingsRecordLookup lookup = g_settingsRecordLookup.load(std::memory_order_acquire);
    if (lookup == nullptr || playerIndex == teleport::kInvalidHandle) {
        return false;
    }
    std::byte* const settings = lookup(playerIndex);
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(settings);
    if (value == 0 || value > (std::numeric_limits<std::uintptr_t>::max)() - kHudOpacityOffset) {
        return false;
    }
    address = reinterpret_cast<std::byte*>(value + kHudOpacityOffset);
    return committed_address(address);
}

/** Captures and authors native HUD opacity while releasing ownership to any newer writer. */
[[nodiscard]] bool suppress_hud() noexcept {
    if (g_settingsRecordLookup.load(std::memory_order_acquire) == nullptr || g_hud.released) {
        return true;
    }
    std::byte* address{};
    std::uint8_t current{};
    if (!hud_opacity_address(g_hud.playerIndex, address) || !read_at(address, current)
        || current > kHudOpacityMaximum) {
        g_hudHidden.store(false, std::memory_order_release);
        g_hudFault.store(true, std::memory_order_release);
        if (!g_hud.faultReported) {
            report("hud", "fault", "lookup_or_read");
            g_hud.faultReported = true;
        }
        return false;
    }
    if (!g_hud.captured) {
        g_hud.prior = current;
        g_hud.captured = true;
    } else if ((g_hud.authored && current != kHudOpacityOff)
               || (!g_hud.authored && current != g_hud.prior)) {
        g_hud.captured = false;
        g_hud.released = true;
        g_hudHidden.store(false, std::memory_order_release);
        g_hudFault.store(false, std::memory_order_release);
        report("hud", "released", "newer_value");
        return true;
    }
    if (current != kHudOpacityOff && !write_at(address, kHudOpacityOff)) {
        g_hudHidden.store(false, std::memory_order_release);
        g_hudFault.store(true, std::memory_order_release);
        if (!g_hud.faultReported) {
            report("hud", "fault", "write");
            g_hud.faultReported = true;
        }
        return false;
    }
    g_hud.authored = true;
    g_hudHidden.store(true, std::memory_order_release);
    g_hudFault.store(false, std::memory_order_release);
    if (!g_hud.suppressedReported) {
        report("hud", "ok", "suppressed");
        g_hud.suppressedReported = true;
    }
    return true;
}

/** Restores the captured HUD value only while Sunrise still owns the native byte. */
[[nodiscard]] bool restore_hud() noexcept {
    if (!g_hud.captured) {
        g_hudHidden.store(false, std::memory_order_release);
        g_hudFault.store(false, std::memory_order_release);
        return true;
    }
    std::byte* address{};
    std::uint8_t current{};
    if (!hud_opacity_address(g_hud.playerIndex, address) || !read_at(address, current)) {
        g_hudFault.store(true, std::memory_order_release);
        if (!g_hud.faultReported) {
            report("hud", "fault", "restore");
            g_hud.faultReported = true;
        }
        return false;
    }
    if (current != kHudOpacityOff) {
        g_hud.captured = false;
        g_hudHidden.store(false, std::memory_order_release);
        g_hudFault.store(false, std::memory_order_release);
        report("hud", "released", "newer_value");
        return true;
    }
    if (!write_at(address, g_hud.prior)) {
        g_hudFault.store(true, std::memory_order_release);
        if (!g_hud.faultReported) {
            report("hud", "fault", "restore");
            g_hud.faultReported = true;
        }
        return false;
    }
    g_hud.captured = false;
    g_hudHidden.store(false, std::memory_order_release);
    g_hudFault.store(false, std::memory_order_release);
    report("hud", "ok", "restored");
    return true;
}

/** Drops HUD ownership metadata after restoration or a player change. */
void reset_hud_session() noexcept {
    g_hud = {};
}

/** Counts replacement frames so detour removal never unmaps executing code. */
struct ReplacementScope {
    ReplacementScope() noexcept {
        g_replacementInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ReplacementScope() {
        g_replacementInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
    ReplacementScope(const ReplacementScope&) = delete;
    ReplacementScope& operator=(const ReplacementScope&) = delete;
};

/** @return True when no replacement frame remains on a thread stack. */
[[nodiscard]] bool replacements_idle() noexcept {
    return g_replacementInFlight.load(std::memory_order_acquire) == 0;
}

/** Waits only for install publication, then returns the native trampoline. */
[[nodiscard]] FirstPersonRigTransformSubmit original_rig_transform() noexcept {
    FirstPersonRigTransformSubmit next = g_originalRigTransform.load(std::memory_order_acquire);
    while (next == nullptr && g_installPublishing.load(std::memory_order_acquire)) {
        SwitchToThread();
        next = g_originalRigTransform.load(std::memory_order_acquire);
    }
    return next;
}

/** Redirects only the validated first-person caller's render transform. */
__declspec(noinline) void __fastcall
submit_first_person_rig_transform(std::byte* destination,
                                  const FirstPersonRigTransform* transform) noexcept {
    ReplacementScope scope{};
    const FirstPersonRigTransformSubmit next = original_rig_transform();
    if (next == nullptr) {
        return;
    }
    const bool suppress = _ReturnAddress() == g_rigTransformReturn.load(std::memory_order_acquire)
                          && g_hideWeaponRequested.load(std::memory_order_acquire);
    FirstPersonRigTransform redirected{};
    if (!suppress || !read_at(reinterpret_cast<const std::byte*>(transform), redirected)
        || !finite(redirected.orientation) || !finite(redirected.position)) {
        next(destination, transform);
        return;
    }
    redirected.position[0] += kHiddenRigOffset;
    next(destination, &redirected);
    g_weaponHidden.store(true, std::memory_order_release);
}

/** Resolves all HUD targets and proves their caller/field relationships. */
[[nodiscard]] bool resolve_hud_target() noexcept {
    std::byte* const caller =
        patterns::scan_main_image_unique(kHudRowCaller, "presentation_hud_row_caller");
    std::byte* const lookup = patterns::scan_main_image_unique(
        kSettingsRecordLookup, "presentation_settings_record_lookup");
    std::byte* const reader =
        patterns::scan_main_image_unique(kHudRowReader, "presentation_hud_row_reader");
    std::byte* const item20 =
        patterns::scan_main_image_unique(kHudItem20, "presentation_hud_item_20");
    if (caller == nullptr || lookup == nullptr || reader == nullptr || item20 == nullptr
        || item20 != reader + kHudItem20InRowReader
        || call_target<SettingsRecordLookup>(caller, kHudLookupCall)
               != reinterpret_cast<SettingsRecordLookup>(lookup)
        || call_target<HudRowReader>(caller, kHudRowReaderCall)
               != reinterpret_cast<HudRowReader>(reader)) {
        report("hud", "unavailable", "structural_validation");
        return false;
    }
    g_settingsRecordLookup.store(reinterpret_cast<SettingsRecordLookup>(lookup),
                                 std::memory_order_release);
    return true;
}

} // namespace

/** Resolves the rig/HUD targets and attaches the first-person transform detour. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_handle.attached) {
        report("install", "fail", "detour_attached");
        return false;
    }
    g_stopping.store(false, std::memory_order_release);
    std::byte* const rigUpdate = patterns::scan_main_image_unique(
        kFirstPersonRigUpdate, "presentation_first_person_rig_update");
    if (rigUpdate == nullptr) {
        report("install", "fail", "rig_update_pattern");
        return false;
    }
    const FirstPersonRigTransformSubmit rigTransform =
        call_target<FirstPersonRigTransformSubmit>(rigUpdate, kFirstPersonRigTransformCall);
    if (rigTransform == nullptr
        || !matches_signature(reinterpret_cast<const std::byte*>(rigTransform),
                              kFirstPersonRigTransform)) {
        report("install", "fail", "rig_transform_structure");
        return false;
    }
    if (rip_target(reinterpret_cast<std::byte*>(rigTransform) + kFirstPersonRigGlobalLoad)
        == nullptr) {
        report("install", "fail", "rig_transform_global");
        return false;
    }

    g_settingsRecordLookup.store(nullptr, std::memory_order_release);
    const bool hudReady = resolve_hud_target();
    g_rigTransformReturn.store(rigUpdate + kFirstPersonRigTransformCall + 5,
                               std::memory_order_release);
    g_installPublishing.store(true, std::memory_order_release);
    if (!hooking::detour::install(
            hooking::detour::Spec{rigTransform,
                                  reinterpret_cast<void*>(&submit_first_person_rig_transform)},
            g_handle)) {
        g_installPublishing.store(false, std::memory_order_release);
        g_rigTransformReturn.store(nullptr, std::memory_order_release);
        g_settingsRecordLookup.store(nullptr, std::memory_order_release);
        report("install", "fail", "detour_attach");
        return false;
    }
    g_originalRigTransform.store(reinterpret_cast<FirstPersonRigTransformSubmit>(g_handle.original),
                                 std::memory_order_release);
    g_hideWeaponRequested.store(false, std::memory_order_release);
    g_removeHudRequested.store(false, std::memory_order_release);
    g_weaponHidden.store(false, std::memory_order_release);
    g_hudHidden.store(false, std::memory_order_release);
    g_hudFault.store(false, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    g_installPublishing.store(false, std::memory_order_release);
    report("install", hudReady ? "ok" : "partial", hudReady ? nullptr : "hud_unavailable");
    return true;
}

/** Restores HUD ownership before safely detaching the rig-transform replacement. */
bool uninstall() noexcept {
    g_stopping.store(true, std::memory_order_release);
    g_hideWeaponRequested.store(false, std::memory_order_release);
    g_removeHudRequested.store(false, std::memory_order_release);
    g_weaponHidden.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_hudLock);
    const bool restored = restore_hud();
    if (restored) {
        reset_hud_session();
    }
    ReleaseSRWLockExclusive(&g_hudLock);
    if (!restored) {
        report("uninstall", "wait", "hud_restore");
        return false;
    }

    if (g_handle.attached) {
        const hooking::detour::ProtectedCodeEntry protectedEntry{
            reinterpret_cast<void*>(&submit_first_person_rig_transform)};
        const hooking::detour::UninstallResult removal = hooking::detour::uninstall(
            std::span(&g_handle, 1), std::span(&protectedEntry, 1), &replacements_idle);
        if (removal != hooking::detour::UninstallResult::removed) {
            report("uninstall",
                   removal == hooking::detour::UninstallResult::protectedCodeActive ? "wait"
                                                                                    : "fail",
                   removal == hooking::detour::UninstallResult::protectedCodeActive
                       ? "replacement_active"
                       : "detour_detach");
            return false;
        }
    }

    g_installed.store(false, std::memory_order_release);
    g_originalRigTransform.store(nullptr, std::memory_order_release);
    g_settingsRecordLookup.store(nullptr, std::memory_order_release);
    g_rigTransformReturn.store(nullptr, std::memory_order_release);
    g_handle = {};
    report("uninstall", "ok");
    return true;
}

/** Applies the persisted presentation policies after the stock camera frame. */
void apply(std::uint32_t playerIndex) noexcept {
    if (!g_installed.load(std::memory_order_acquire)
        || g_stopping.load(std::memory_order_acquire)) {
        return;
    }
    const client::player::Settings settings = client::player::get();
    const bool hideWeapon = settings.hideWeapon;
    const bool removeHud = settings.removeHud;
    g_hideWeaponRequested.store(hideWeapon, std::memory_order_release);
    const bool removeHudWasRequested =
        g_removeHudRequested.exchange(removeHud, std::memory_order_acq_rel);
    if (!hideWeapon) {
        g_weaponHidden.store(false, std::memory_order_release);
    }
    if (!removeHud && !removeHudWasRequested) {
        return;
    }

    AcquireSRWLockExclusive(&g_hudLock);
    if (g_hud.playerIndex != teleport::kInvalidHandle && g_hud.playerIndex != playerIndex) {
        if (!restore_hud()) {
            ReleaseSRWLockExclusive(&g_hudLock);
            return;
        }
        reset_hud_session();
    }
    if (playerIndex == teleport::kInvalidHandle || !teleport::local_player_available()) {
        if (!removeHud && restore_hud()) {
            reset_hud_session();
        }
        ReleaseSRWLockExclusive(&g_hudLock);
        return;
    }
    if (g_hud.playerIndex == teleport::kInvalidHandle) {
        g_hud.playerIndex = playerIndex;
    }
    if (removeHud) {
        (void)suppress_hud();
    } else if (restore_hud()) {
        reset_hud_session();
    }
    ReleaseSRWLockExclusive(&g_hudLock);
}

/** @return One lock-free snapshot for the HUD page. */
Status status() noexcept {
    return Status{g_installed.load(std::memory_order_acquire),
                  g_settingsRecordLookup.load(std::memory_order_acquire) != nullptr,
                  g_hideWeaponRequested.load(std::memory_order_acquire),
                  g_removeHudRequested.load(std::memory_order_acquire),
                  g_weaponHidden.load(std::memory_order_acquire),
                  g_hudHidden.load(std::memory_order_acquire),
                  g_hudFault.load(std::memory_order_acquire)};
}

} // namespace sunrise::client::hooks::presentation
