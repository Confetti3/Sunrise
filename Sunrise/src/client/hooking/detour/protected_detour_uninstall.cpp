#include <Windows.h>

#include <array>

#include "../detour.h"
#include "transaction/detour_thread_transaction.h"

namespace sunrise::client::hooking::detour {
namespace {

using transaction::kBatchLimit;

} // namespace

/**
 * Spans cover code with no unwind entry, such as finally blocks.
 * @param handle Attached detour handle.
 * @param protectedEntries Function entries that must be idle before removal.
 * @param protectedSpans Executable image intervals with exclusive ends.
 * @param idleCheck Lock-free idle test run with transaction threads suspended, or null.
 * @return Removed, active, or failed without changing the handle on refusal.
 */
UninstallResult uninstall(Handle& handle,
                          std::span<const ProtectedCodeEntry> protectedEntries,
                          std::span<const ProtectedCodeSpan> protectedSpans,
                          IdleCheck idleCheck) noexcept {
    return uninstall(std::span(&handle, 1), protectedEntries, protectedSpans, idleCheck);
}

/**
 * Spans cover code with no unwind entry, such as finally blocks.
 * @param handles Attached handles in their original transaction order.
 * @param protectedEntries Function entries that must be idle before removal.
 * @param protectedSpans Executable image intervals with exclusive ends.
 * @param idleCheck Lock-free idle test run with transaction threads suspended, or null.
 * @return Removed, active, or failed; refusal leaves all handles attached.
 */
UninstallResult uninstall(std::span<Handle> handles,
                          std::span<const ProtectedCodeEntry> protectedEntries,
                          std::span<const ProtectedCodeSpan> protectedSpans,
                          IdleCheck idleCheck) noexcept {
    if (!transaction::valid_detach_batch(handles)
        || (protectedEntries.empty() && protectedSpans.empty())) {
        return UninstallResult::failed;
    }

    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return UninstallResult::failed;
    }

    const transaction::InspectionResult inspection =
        transaction::inspect(threads, protectedEntries, protectedSpans);
    if (inspection != transaction::InspectionResult::clear) {
        const bool aborted = transaction::abort(threads);
        if (!aborted || inspection == transaction::InspectionResult::failed) {
            return UninstallResult::failed;
        }
        return UninstallResult::protectedCodeActive;
    }
    if (idleCheck != nullptr && !idleCheck()) {
        return transaction::abort(threads) ? UninstallResult::protectedCodeActive
                                           : UninstallResult::failed;
    }

    std::array<void*, kBatchLimit> originals{};
    if (!transaction::queue_detach(handles, originals)) {
        (void)transaction::abort(threads);
        return UninstallResult::failed;
    }
    if (!transaction::commit(threads)) {
        return UninstallResult::failed;
    }

    transaction::clear_handles(handles);
    return UninstallResult::removed;
}

} // namespace sunrise::client::hooking::detour
