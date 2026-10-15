#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"
#include "studio_cui_link.h"
#include "studio_cui_reader.h"

namespace sunrise::client::diagnostics::studio {
namespace {

/** The UI frame function: UI update, a ready-byte test, then the draw-list build. */
constexpr std::string_view kFrameText =
    "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 80 3D ? ? ? ? 00 74 1D 48 85 DB 74 18 "
    "48 8D 0D ? ? ? ? 48 8B D3 48 83 E1 F0";
constexpr auto kFrame = patterns::signature<patterns::signature_length(kFrameText)>(kFrameText);
/** The ready-byte compare's operand is bytes 16..19; the manager lea's operand is 31..34. */
constexpr std::size_t kReadyOperand = 16, kReadyNext = 21;
constexpr std::size_t kManagerOperand = 31, kManagerNext = 35;
/** The frame masks the lea target down to the manager's 16-byte alignment. */
constexpr std::uintptr_t kManagerAlignMask = ~std::uintptr_t{15};
using Frame = std::uintptr_t(__fastcall*)(std::uintptr_t);

hooking::detour::Handle g_hook{};
volatile LONG g_active{};
std::atomic_bool g_threadLogged{false};

void log_install(std::string_view result) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "ev=studio stage=cui result=%.*s",
                      static_cast<int>(result.size()),
                      result.data());
}

/** Calls the frame first, so every read sees this frame's finished UI state. */
__declspec(noinline) std::uintptr_t __fastcall observe_body(std::uintptr_t ui) noexcept {
    const auto original = reinterpret_cast<Frame>(g_hook.original);
    const std::uintptr_t result = original(ui);
    if (observe_cui_frame(GetTickCount64()) && !g_threadLogged.exchange(true)) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=studio stage=cui thread=%lu",
                          GetCurrentThreadId());
    }
    return result;
}

#pragma code_seg(push, cui_guard_body, ".srcui")
/** Every borrowed native or helper call finishes before this protected decrement. */
__declspec(noinline) void egress() noexcept {
    (void)InterlockedDecrement(&g_active);
}

/** The original call and native faults cross this boundary without changing their meaning. */
__declspec(noinline) std::uintptr_t __fastcall observe(std::uintptr_t ui) noexcept {
    (void)InterlockedIncrement(&g_active);
    std::uintptr_t result{};
    __try {
        result = observe_body(ui);
    } __finally {
        egress();
    }
    return result;
}
#pragma code_seg(pop, cui_guard_body)

/** Detours calls this while peer threads are suspended, so it must not acquire a lock. */
[[nodiscard]] bool idle() noexcept {
    return InterlockedCompareExchange(&g_active, 0, 0) == 0;
}

} // namespace

/** Any failure leaves the observer detached and logs the reason once. */
bool initialize_cui_link() noexcept {
    if (g_hook.attached) {
        start_cui_mapping();
        return true;
    }
    std::byte* const match = patterns::scan_main_image_unique(kFrame, "studio_cui_frame");
    if (match == nullptr) {
        log_install("fail reason=pattern");
        return false;
    }
    const auto ready = reinterpret_cast<std::uintptr_t>(
        patterns::resolve_relative(match + kReadyOperand, match + kReadyNext));
    const auto manager = reinterpret_cast<std::uintptr_t>(patterns::resolve_relative(
                             match + kManagerOperand, match + kManagerNext))
                         & kManagerAlignMask;
    if (!initialize_cui_reader(manager, ready)) {
        log_install("fail reason=manager");
        return false;
    }
    if (!hooking::detour::install({match, reinterpret_cast<void*>(&observe)}, g_hook)) {
        clear_cui_reader();
        log_install("fail reason=detour");
        return false;
    }
    g_threadLogged.store(false);
    start_cui_mapping();
    log_install("ok");
    return true;
}

/** An active observer keeps its trampoline attached but inert until a later attempt. */
bool close_cui_link() noexcept {
    close_cui_mapping();
    if (g_hook.attached) {
        const std::array entries{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe)}};
        // .srcui holds the observer and its compiler continuations.
        const std::array spans{hooking::detour::section_span(".srcui")};
        if (hooking::detour::uninstall(g_hook, entries, spans, &idle)
            != hooking::detour::UninstallResult::removed) {
            return false;
        }
    }
    clear_cui_reader();
    return true;
}

} // namespace sunrise::client::diagnostics::studio
