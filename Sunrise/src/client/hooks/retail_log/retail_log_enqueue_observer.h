#pragma once

#include <Windows.h>

#include <cstdint>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::retail_log {

extern SRWLOCK g_lock;
extern hooking::detour::Handle g_handle;

struct SobjectCaptureStatus final {
    std::uint64_t generation{};
    std::uint32_t backgroundFailures{};
    std::uint32_t recentFailures{};
    bool wideArmed{};
    bool wideCapturing{};
    bool wideRearming{};
};

/** Re-arms the one-shot wide code capture without restarting the client. */
[[nodiscard]] std::uint64_t rearm_sobject_capture() noexcept;

/** Copies bounded capture counters for the runtime UI. */
[[nodiscard]] SobjectCaptureStatus sobject_capture_status() noexcept;

/** Captures the containing function, or a bounded `manual_raw` window when unwind data is absent. */
[[nodiscard]] std::uint64_t capture_sobject_function(std::uintptr_t rva) noexcept;

/** @return The enqueue observer body itself, with internal linkage. */
[[nodiscard]] void* enqueue_entry_point() noexcept;

/** Applies the configured category threshold, once we know the log block exists. */
void assert_verbosity() noexcept;

} // namespace sunrise::client::hooks::retail_log
