#include <Windows.h>

#include <string_view>

#include "../registry/ui_module_registry.h"
#include "../ui_module_descriptor.h"
#include "internal.h"
#include "logs.h"

namespace sunrise::core::ui::modules::logs {
namespace {

/** Namespaced stable ID keeps this page distinct from feature modules. */
constexpr std::string_view kStableId = "core.logs";
/** Menu label for the process-wide log view. */
constexpr std::string_view kDisplayName = "Logs";

registry::PageRegistration g_page;

} // namespace

/** @return True when the Core Logs page owns its registry slot. */
bool initialize() noexcept {
    // The filter state is cleared under the slot lock, so a re-register never draws stale filters.
    return g_page.acquire(Owner::core, kStableId, kDisplayName, &internal::draw, &internal::reset);
}

/** Removes the Core Logs page and clears its local filter state. */
void shutdown() noexcept {
    g_page.release(&internal::reset);
}

/** Queues one text payload for deferred Win32 clipboard delivery. */
bool queue_text_copy(std::string_view text) noexcept {
    return internal::request_copy(text);
}

/** Posts the existing deferred-action message once for pending clipboard work. */
void notify_pending_copy(HWND owner, UINT message) noexcept {
    internal::notify_pending_copy(owner, message);
}

/** @param owner Active game output window that becomes the clipboard owner. */
void dispatch_pending_copy(HWND owner) noexcept {
    internal::dispatch_pending_copy(owner);
}

} // namespace sunrise::core::ui::modules::logs
