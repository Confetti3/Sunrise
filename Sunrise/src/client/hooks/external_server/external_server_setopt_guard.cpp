/**
 * Guard on `curl_easy_setopt`, attached only in external-server mode. It forces the pinned TLS
 * peer checks off, and rewrites every request URL's host to the external server. The rewrite is
 * the catch-all for request paths the HTTP executor never sees.
 */

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "internal.h"
#include "route.h"

namespace sunrise::client::hooks::external_server {
namespace {

using Setopt = int (*)(void*, int, void*) noexcept;

hooking::detour::Handle g_handle{};
SRWLOCK g_lock{SRWLOCK_INIT};
volatile LONG g_activeCalls{};

/** curl reports a missing easy-setopt implementation with CURLE_FAILED_INIT. */
constexpr int kFailedInit = 2;

#if defined(SUNRISE_SETOPT_TEST)
volatile LONG g_pauseIngress{};
volatile LONG g_ingressEntered{};
volatile LONG g_releaseIngress{1};
volatile LONG g_pausePublish{};
volatile LONG g_publishEntered{};
volatile LONG g_releasePublish{1};
volatile LONG g_pauseFinally{};
volatile LONG g_finallyEntered{};
volatile LONG g_releaseFinally{1};
/** A failed fixture cannot keep its callback spinning forever. */
constexpr unsigned kIngressPauseAttempts = 1'000'000'000;
#endif

#pragma code_seg(push, setopt_guard, ".sropt")
/**
 * Count before borrowing; attach publishes the trampoline under the same lock.
 * @return The trampoline held until egress.
 */
[[nodiscard]] __declspec(noinline) Setopt ingress() noexcept {
#if defined(SUNRISE_SETOPT_TEST)
    if (InterlockedCompareExchange(&g_pauseIngress, 0, 1) == 1) {
        (void)InterlockedExchange(&g_ingressEntered, 1);
        for (unsigned attempt = 0; attempt < kIngressPauseAttempts && g_releaseIngress == 0;
             ++attempt) {
            YieldProcessor();
        }
    }
#endif
    (void)InterlockedIncrement(&g_activeCalls);
    AcquireSRWLockShared(&g_lock);
    const auto original = reinterpret_cast<Setopt>(g_handle.original);
    ReleaseSRWLockShared(&g_lock);
    return original;
}

/** The last decrement stays inside protected code, after every borrowed call has returned. */
__declspec(noinline) void egress() noexcept {
    (void)InterlockedDecrement(&g_activeCalls);
}
#pragma code_seg(pop, setopt_guard)

/**
 * Suspended peers may own locks, so this check must not acquire one.
 * @return True when no counted callback remains.
 */
[[nodiscard]] bool idle() noexcept {
    return InterlockedCompareExchange(&g_activeCalls, 0, 0) == 0;
}

/**
 * The URL buffer must survive the caller's later trampoline call.
 * @param value Caller-owned null-terminated URL.
 * @return Replacement URL, or the caller's own when it was not rewritten.
 */
[[nodiscard]] void* redirect_url(void* value) noexcept {
    thread_local std::array<char, kUrlCapacity> held{};
    const auto* const text = static_cast<const char*>(value);
    const std::string_view original(text, strnlen_s(text, kUrlCapacity));
    const std::size_t size = rewrite_host(original, held);
    core::log::writef(core::log::Channel::client,
                      core::log::Level::debug,
                      "ev=external stage=setopt_url from=%.*s to=%.*s result=%s",
                      static_cast<int>(original.size()),
                      original.data(),
                      static_cast<int>(size),
                      held.data(),
                      size == 0 ? "unchanged" : "ok");
    return size != 0 ? held.data() : value;
}

/**
 * The borrowed trampoline remains valid through every nested call.
 * @param original Trampoline borrowed by the current counted call.
 * @param handle Borrowed curl easy handle.
 * @param option curl option id.
 * @param value Caller-supplied option value.
 * @return The original result, or CURLE_FAILED_INIT when there is no trampoline.
 */
__declspec(noinline) int
setopt_body(Setopt original, void* handle, int option, void* value) noexcept {
    if (original == nullptr) {
        return kFailedInit;
    }
    if (option == kSslVerifyPeer || option == kSslVerifyHost) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::debug,
                          "ev=external stage=unpin option=%d value=0 result=ok",
                          option);
        return original(handle, option, nullptr);
    }
    if (option == kUrl && value != nullptr) {
        return original(handle, option, redirect_url(value));
    }
    return original(handle, option, value);
}

#pragma code_seg(push, setopt_guard, ".sropt")
/**
 * The counted lease covers native callees and unwind through the setter.
 * @param handle Borrowed curl easy handle.
 * @param option Curl option id.
 * @param value Caller-owned option value.
 * @return The original setter result.
 */
__declspec(noinline) int setopt(void* handle, int option, void* value) noexcept {
    const Setopt original = ingress();
    int result = kFailedInit;
    __try {
        result = setopt_body(original, handle, option, value);
    } __finally {
        egress();
#if defined(SUNRISE_SETOPT_TEST)
        if (InterlockedCompareExchange(&g_pauseFinally, 0, 1) == 1) {
            (void)InterlockedExchange(&g_finallyEntered, 1);
            for (unsigned attempt = 0; attempt < kIngressPauseAttempts && g_releaseFinally == 0;
                 ++attempt) {
                YieldProcessor();
            }
        }
#endif
    }
    return result;
}
#pragma code_seg(pop, setopt_guard)

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail_install(const char* reason) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=external stage=install result=fail reason=%s",
                      reason);
    return false;
}

} // namespace

/** @return False if the enabled guard cannot be attached; unchanged when disabled. */
bool install() noexcept {
    if (!enabled()) {
        return true;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_handle.attached) {
        ReleaseSRWLockExclusive(&g_lock);
        return true;
    }
    std::byte* const target = scan_main_image_unique(kSetoptSignature, "curl_easy_setopt");
    if (target == nullptr) {
        ReleaseSRWLockExclusive(&g_lock);
        return fail_install("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&setopt)};
    if (!hooking::detour::install(spec, g_handle)) {
        ReleaseSRWLockExclusive(&g_lock);
        return fail_install("attach");
    }
#if defined(SUNRISE_SETOPT_TEST)
    if (InterlockedCompareExchange(&g_pausePublish, 0, 1) == 1) {
        (void)InterlockedExchange(&g_publishEntered, 1);
        for (unsigned attempt = 0; attempt < kIngressPauseAttempts && g_releasePublish == 0;
             ++attempt) {
            YieldProcessor();
        }
    }
#endif
    ReleaseSRWLockExclusive(&g_lock);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=external stage=install result=ok");
    return true;
}

/** @return True only after the guard and every borrowed trampoline call are gone. */
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    // .sropt holds ingress, egress, setopt and its finally block.
    const std::array spans{hooking::detour::section_span(".sropt")};
    const bool removed = idle()
                         && (!g_handle.attached
                             || hooking::detour::uninstall(g_handle, {}, spans, &idle)
                                    == hooking::detour::UninstallResult::removed);
    ReleaseSRWLockExclusive(&g_lock);
    return removed;
}

/** @return True while the setopt guard is attached. */
bool is_installed() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool attached = g_handle.attached;
    ReleaseSRWLockShared(&g_lock);
    return attached;
}

} // namespace sunrise::client::hooks::external_server
