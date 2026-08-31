#include <Windows.h>

#include "../../../core/ui/busy/busy.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../items/packages/build.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::content::investment {
namespace {

SRWLOCK g_refreshLock{SRWLOCK_INIT};

[[nodiscard]] bool ready() noexcept {
    return state::build_data::named_catalog_ready() && items::packages::ready();
}

} // namespace

/** @return True when the next refresh slice needs a visible overlay for a package sweep. */
bool requires_package_sweep() noexcept {
    return !state::build_data::item_definitions_ready() && items::packages::readable();
}

/** Publishes every installed equipment mapping domain. */
bool refresh() noexcept {
    if (ready()) {
        // The same lock as the extraction path. A cache write holds its own lock across file
        // calls, so a held thread stopped inside one would deadlock the freeze below.
        AcquireSRWLockExclusive(&g_refreshLock);
        const bool persisted = state::ensure_profile_item_identities()
                               && state::ensure_character_subclasses()
                               && state::build_data::persist();
        // Nothing reads a package again until the next boot, so the open files and the held
        // tables go back now rather than at process exit.
        middleware::content::packages::reader::release_caches();
        ReleaseSRWLockExclusive(&g_refreshLock);
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
        return persisted;
    }

    AcquireSRWLockExclusive(&g_refreshLock);
    // The package pass creates parallel readers. Suspending the client while those threads start
    // can block their DLL thread-attach work behind a suspended owner, so the visible preflight
    // runs one frame early and extraction proceeds with the process live.
    core::ui::busy::raise(core::ui::busy::Task::contentExtraction);
    // The package pass owns the item table and must not wait on runtime content lookups.
    (void)items::packages::build();
    const bool domainsReady = ready();
    const bool complete = domainsReady && state::ensure_profile_item_identities()
                          && state::ensure_character_subclasses() && state::build_data::persist();
    // The overlay ends with the work, not with the slice, so it spans every retry the pass needs.
    if (complete) {
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
    }
    ReleaseSRWLockExclusive(&g_refreshLock);
    return complete;
}

} // namespace sunrise::client::content::investment
