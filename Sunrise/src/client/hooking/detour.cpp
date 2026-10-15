#include "detour.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <detours.h>

#include "detour/transaction/detour_thread_transaction.h"

namespace sunrise::client::hooking::detour {
namespace {

using transaction::clear_handles;
using transaction::kBatchLimit;
using transaction::queue_detach;

/**
 * Checks every detour description and output slot before transaction setup.
 * @param specs Target and replacement pairs in stable slot order.
 * @param outputs Output handles paired with the descriptions.
 * @return True when the batch can be attached without overwriting state.
 */
[[nodiscard]] bool valid_install_batch(std::span<const Spec> specs,
                                       std::span<const Handle> outputs) noexcept {
    if (specs.empty() || specs.size() != outputs.size() || specs.size() > kBatchLimit) {
        return false;
    }
    for (std::size_t index = 0; index < specs.size(); ++index) {
        if (outputs[index].attached || specs[index].target == nullptr
            || specs[index].replacement == nullptr) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Attaches one detour through the batch transaction path.
 * @param spec Target and replacement pair.
 * @param output Receives the committed trampoline.
 * @return True when the detour is attached.
 */
bool install(const Spec& spec, Handle& output) noexcept {
    return install(std::span(&spec, 1), std::span(&output, 1));
}

/**
 * Detaches one detour without caller-owned protected function checks.
 * @param handle Attached detour handle.
 * @return True when the detour is detached.
 */
bool uninstall(Handle& handle) noexcept {
    return uninstall(std::span(&handle, 1));
}

/**
 * Detaches one hook unless a suspended instruction pointer sits inside its replacement.
 * @param handle Attached detour handle.
 * @param replacementActive Receives true when the replacement prevented removal.
 * @return True when the detour is detached.
 */
bool uninstall(Handle& handle, bool& replacementActive) noexcept {
    return uninstall(std::span(&handle, 1), replacementActive);
}

/**
 * Attaches a checked detour batch in one transaction, all or nothing.
 * @param specs Target and replacement pairs in stable slot order.
 * @param outputs Stable slots that Detours fills before resuming patched threads.
 * @return True when every detour is attached.
 */
bool install(std::span<const Spec> specs, std::span<Handle> outputs) noexcept {
    if (!valid_install_batch(specs, outputs)) {
        return false;
    }

    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return false;
    }

    for (std::size_t index = 0; index < specs.size(); ++index) {
        outputs[index].original = specs[index].target;
        outputs[index].replacement = specs[index].replacement;
        if (DetourAttach(&outputs[index].original, specs[index].replacement) != NO_ERROR) {
            // Abort keeps every output detached when one attachment is rejected.
            (void)transaction::abort(threads);
            clear_handles(outputs);
            return false;
        }
    }
    if (!transaction::commit(threads)) {
        clear_handles(outputs);
        return false;
    }

    for (std::size_t index = 0; index < specs.size(); ++index) {
        outputs[index].attached = true;
    }
    return true;
}

/**
 * Detaches a checked batch without caller-owned protected function checks.
 * @param handles Attached handles in their original transaction order.
 * @return True when every detour is removed and every handle is cleared.
 */
bool uninstall(std::span<Handle> handles) noexcept {
    if (!transaction::valid_detach_batch(handles)) {
        return false;
    }

    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return false;
    }

    std::array<void*, kBatchLimit> originals{};
    if (!queue_detach(handles, originals)) {
        (void)transaction::abort(threads);
        return false;
    }
    if (!transaction::commit(threads)) {
        return false;
    }

    clear_handles(handles);
    return true;
}

/**
 * Detaches a batch unless a suspended instruction pointer sits inside a replacement.
 * @param handles Attached handles in their original transaction order.
 * @param replacementActive Receives true when a replacement prevented removal.
 * @return True when every detour is removed and every handle is cleared.
 */
bool uninstall(std::span<Handle> handles, bool& replacementActive) noexcept {
    replacementActive = false;
    if (handles.empty() || handles.size() > kBatchLimit) {
        return false;
    }

    std::array<ProtectedCodeEntry, kBatchLimit> entries{};
    for (std::size_t index = 0; index < handles.size(); ++index) {
        entries[index].address = handles[index].replacement;
    }
    const UninstallResult result = uninstall(handles, std::span(entries.data(), handles.size()));
    replacementActive = result == UninstallResult::protectedCodeActive;
    return result == UninstallResult::removed;
}

/**
 * Reads the section table of this module's own headers.
 * @param name Section name, at most 8 characters.
 * @return Section bounds, or an empty span so removal fails closed.
 */
ProtectedCodeSpan section_span(const char* name) noexcept {
    HMODULE self{};
    // From this function's own address, so it names this DLL, not the game.
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&section_span),
                           &self)
            == FALSE
        || self == nullptr) {
        return {};
    }
    const auto* const base = reinterpret_cast<const std::byte*>(self);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* const first = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::byte*>(&nt->OptionalHeader)
        + nt->FileHeader.SizeOfOptionalHeader);
    for (const IMAGE_SECTION_HEADER& section : std::span(first, nt->FileHeader.NumberOfSections)) {
        const auto* const sectionName = reinterpret_cast<const char*>(section.Name);
        if (std::strncmp(sectionName, name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
            const std::byte* const begin = base + section.VirtualAddress;
            return {begin, begin + section.Misc.VirtualSize};
        }
    }
    return {};
}

} // namespace sunrise::client::hooking::detour
