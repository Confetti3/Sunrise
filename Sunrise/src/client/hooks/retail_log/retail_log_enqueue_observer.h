#pragma once

#include <Windows.h>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::retail_log {

extern hooking::detour::Handle g_handle;

/** @return The enqueue observer body itself, with internal linkage. */
[[nodiscard]] void* enqueue_entry_point() noexcept;

/** Opens every game log category while client debug logging is on. */
void assert_verbosity() noexcept;

} // namespace sunrise::client::hooks::retail_log
