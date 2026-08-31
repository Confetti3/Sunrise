#include "console_registry.h"

#include <Windows.h>

namespace sunrise::core::console::registry {
namespace {

std::array<Descriptor, kEntryCapacity> g_entries{};
std::array<RegistrationId, kEntryCapacity> g_registrationIds{};
std::size_t g_count{};
std::uint64_t g_revision{};
RegistrationId g_lastRegistrationId{};
SRWLOCK g_registryLock{SRWLOCK_INIT};

/**
 * Finds where a name belongs in the sorted table.
 *
 * The table is held sorted at registration, so this is both the lookup and the insertion point.
 * The caller holds the lock.
 *
 * @param name Name to place.
 * @return Index of the first entry not ordered before the name, which may be the count.
 */
[[nodiscard]] std::size_t lower_bound_index(std::string_view name) noexcept {
    std::size_t low = 0;
    std::size_t high = g_count;
    while (low < high) {
        const std::size_t middle = low + ((high - low) / 2);
        if (g_entries[middle].name < name) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

/** @return True when an entry already carries this exact name. The caller holds the lock. */
[[nodiscard]] bool name_taken(std::string_view name) noexcept {
    const std::size_t index = lower_bound_index(name);
    return index < g_count && g_entries[index].name == name;
}

/**
 * Finds one entry by a name a reader may have typed in any case.
 *
 * Registered names are folded, so the ordered table is also folded and the search that walks it
 * stays correct while comparing case-insensitively.
 *
 * @return Its index, or the count when nothing matches. The caller holds the lock.
 */
[[nodiscard]] std::size_t index_of_folded(std::string_view name) noexcept {
    const std::size_t index = lower_bound_index(name);
    if (index < g_count && equals_folded(g_entries[index].name, name)) {
        return index;
    }
    // A folded query can sort beside its match rather than onto it, so the neighbour is checked
    // before the search gives up.
    if (index > 0 && equals_folded(g_entries[index - 1].name, name)) {
        return index - 1;
    }
    for (std::size_t scan = 0; scan < g_count; ++scan) {
        if (equals_folded(g_entries[scan].name, name)) {
            return scan;
        }
    }
    return g_count;
}

/**
 * Inserts one descriptor at its sorted position. The caller holds the lock and has already
 * established that the name is free and the table has room.
 */
void insert_sorted(const Descriptor& descriptor, RegistrationId registration) noexcept {
    const std::size_t index = lower_bound_index(descriptor.name);
    for (std::size_t position = g_count; position > index; --position) {
        g_entries[position] = g_entries[position - 1];
        g_registrationIds[position] = g_registrationIds[position - 1];
    }
    g_entries[index] = descriptor;
    g_registrationIds[index] = registration;
    ++g_count;
}

/** Issues an identity that is never zero or reused during this process. The caller holds the lock. */
[[nodiscard]] RegistrationId next_registration_id() noexcept {
    ++g_lastRegistrationId;
    if (g_lastRegistrationId == kNoRegistration) {
        ++g_lastRegistrationId;
    }
    return g_lastRegistrationId;
}

} // namespace

/** @return Every registered entry, ordered by name. */
std::span<const Descriptor> RegistrySnapshot::entries() const noexcept {
    return {entries_.data(), count_};
}

/** @return Registry revision captured with the entries. */
std::uint64_t RegistrySnapshot::revision() const noexcept {
    return revision_;
}

/** Copies one descriptor into the fixed table, keeping the table ordered by name. */
RegistrationResult register_entry(const Descriptor& descriptor) noexcept {
    if (!is_complete(descriptor)) {
        return RegistrationResult::incompleteDescriptor;
    }

    AcquireSRWLockExclusive(&g_registryLock);
    if (name_taken(descriptor.name)) {
        ReleaseSRWLockExclusive(&g_registryLock);
        return RegistrationResult::duplicateName;
    }
    if (g_count >= kEntryCapacity) {
        ReleaseSRWLockExclusive(&g_registryLock);
        return RegistrationResult::capacityReached;
    }
    insert_sorted(descriptor, next_registration_id());
    ++g_revision;
    ReleaseSRWLockExclusive(&g_registryLock);
    return RegistrationResult::registered;
}

/** Publishes a whole module's entries as one mutation. */
RegistrationResult register_entries(std::span<const Descriptor> descriptors) noexcept {
    for (const Descriptor& descriptor : descriptors) {
        if (!is_complete(descriptor)) {
            return RegistrationResult::incompleteDescriptor;
        }
    }
    // A name repeated inside one batch is the publisher's own mistake, and catching it here keeps
    // the check below free to look only at what is already registered.
    for (std::size_t outer = 0; outer < descriptors.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < descriptors.size(); ++inner) {
            if (descriptors[outer].name == descriptors[inner].name) {
                return RegistrationResult::duplicateName;
            }
        }
    }

    AcquireSRWLockExclusive(&g_registryLock);
    if (g_count + descriptors.size() > kEntryCapacity) {
        ReleaseSRWLockExclusive(&g_registryLock);
        return RegistrationResult::capacityReached;
    }
    for (const Descriptor& descriptor : descriptors) {
        if (name_taken(descriptor.name)) {
            ReleaseSRWLockExclusive(&g_registryLock);
            return RegistrationResult::duplicateName;
        }
    }
    // Every check passed under this same lock, so no insertion below can fail and the batch lands
    // whole.
    for (const Descriptor& descriptor : descriptors) {
        insert_sorted(descriptor, next_registration_id());
    }
    ++g_revision;
    ReleaseSRWLockExclusive(&g_registryLock);
    return RegistrationResult::registered;
}

/** Removes every entry whose name begins with one module's prefix. */
std::size_t unregister_prefix(std::string_view prefix) noexcept {
    if (prefix.empty()) {
        return 0;
    }

    AcquireSRWLockExclusive(&g_registryLock);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < g_count; ++index) {
        if (g_entries[index].name.starts_with(prefix)) {
            continue;
        }
        // Compaction preserves relative order, so the table stays sorted without a second pass.
        g_entries[kept] = g_entries[index];
        g_registrationIds[kept] = g_registrationIds[index];
        ++kept;
    }
    const std::size_t removed = g_count - kept;
    for (std::size_t index = kept; index < g_count; ++index) {
        g_entries[index] = Descriptor{};
        g_registrationIds[index] = kNoRegistration;
    }
    g_count = kept;
    if (removed != 0) {
        ++g_revision;
    }
    ReleaseSRWLockExclusive(&g_registryLock);
    return removed;
}

/** Finds one entry by its exact name. */
bool find(std::string_view name, Descriptor& output) noexcept {
    RegistrationId ignored = kNoRegistration;
    return resolve(name, output, ignored);
}

/** Resolves one descriptor and its exact registration identity under one lock. */
bool resolve(std::string_view name,
             Descriptor& output,
             RegistrationId& registration) noexcept {
    AcquireSRWLockShared(&g_registryLock);
    const std::size_t index = index_of_folded(name);
    const bool matched = index < g_count;
    if (matched) {
        output = g_entries[index];
        registration = g_registrationIds[index];
    } else {
        registration = kNoRegistration;
    }
    ReleaseSRWLockShared(&g_registryLock);
    return matched;
}

/** Tests whether one exact registration still owns its name. */
bool is_current(std::string_view name, RegistrationId registration) noexcept {
    if (registration == kNoRegistration) {
        return false;
    }
    AcquireSRWLockShared(&g_registryLock);
    const std::size_t index = index_of_folded(name);
    const bool current = index < g_count && g_registrationIds[index] == registration;
    ReleaseSRWLockShared(&g_registryLock);
    return current;
}

/** @return A copy of every entry, taken under the lock, ordered by name. */
RegistrySnapshot snapshot() noexcept {
    RegistrySnapshot copy{};
    AcquireSRWLockShared(&g_registryLock);
    for (std::size_t index = 0; index < g_count; ++index) {
        copy.entries_[index] = g_entries[index];
    }
    copy.count_ = g_count;
    copy.revision_ = g_revision;
    ReleaseSRWLockShared(&g_registryLock);
    return copy;
}

/** Clears every registration. The revision keeps rising. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_registryLock);
    g_entries = {};
    g_registrationIds = {};
    g_count = 0;
    ++g_revision;
    ReleaseSRWLockExclusive(&g_registryLock);
}

/** Clears all storage and resets the revision. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_registryLock);
    g_entries = {};
    g_registrationIds = {};
    g_count = 0;
    g_revision = 0;
    ReleaseSRWLockExclusive(&g_registryLock);
}

} // namespace sunrise::core::console::registry
