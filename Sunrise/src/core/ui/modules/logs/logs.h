#pragma once

#include <Windows.h>

#include <string_view>

namespace sunrise::core::ui::modules::logs {

/** @return True when the Core Logs page owns its registry slot. */
[[nodiscard]] bool initialize() noexcept;

/** Removes the Core Logs page and clears its local filter state. */
void shutdown() noexcept;

/** Queues one text payload for deferred Win32 clipboard delivery. */
[[nodiscard]] bool queue_text_copy(std::string_view text) noexcept;

/** Posts the existing deferred-action message once for a pending clipboard request. */
void notify_pending_copy(HWND owner, UINT message) noexcept;

/**
 * Dispatches one user-requested clipboard copy after presentation locks unwind.
 * @param owner Active game output window that becomes the clipboard owner.
 */
void dispatch_pending_copy(HWND owner) noexcept;

} // namespace sunrise::core::ui::modules::logs
