#include "ui_module_registry.h"

#include <Windows.h>

namespace sunrise::core::ui::modules::registry {
namespace {

struct Slot {
    Descriptor descriptor;
    bool occupied{};
};

constexpr std::size_t kOwnerCount = 3;
constexpr std::array<Owner, kOwnerCount> kMenuOrder{Owner::client, Owner::server, Owner::core};

std::array<Slot, kModuleCapacity> g_slots{};
SRWLOCK g_registryLock{SRWLOCK_INIT};

[[nodiscard]] bool has_id(const Slot& slot, std::string_view stableId) noexcept {
    return slot.descriptor.stable_id() == stableId;
}

[[nodiscard]] bool register_module(const Descriptor& descriptor) noexcept {
    AcquireSRWLockExclusive(&g_registryLock);
    Slot* available = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.occupied && has_id(slot, descriptor.stable_id())) {
            ReleaseSRWLockExclusive(&g_registryLock);
            return false;
        }
        if (!slot.occupied && available == nullptr) {
            // Continue scanning so duplicate IDs still take precedence over capacity.
            available = &slot;
        }
    }
    if (available == nullptr) {
        ReleaseSRWLockExclusive(&g_registryLock);
        return false;
    }
    available->descriptor = descriptor;
    available->occupied = true;
    ReleaseSRWLockExclusive(&g_registryLock);
    return true;
}

void unregister_module(std::string_view stableId) noexcept {
    AcquireSRWLockExclusive(&g_registryLock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && has_id(slot, stableId)) {
            slot = {};
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_registryLock);
}

} // namespace

std::span<const Descriptor> RegistrySnapshot::entries() const noexcept {
    return {entries_.data(), count_};
}

RegistrySnapshot snapshot() noexcept {
    RegistrySnapshot result;
    AcquireSRWLockShared(&g_registryLock);
    // One pass per owner groups the list without sorting it, so registration order is kept
    // inside each group.
    for (const Owner owner : kMenuOrder) {
        for (const Slot& slot : g_slots) {
            if (slot.occupied && slot.descriptor.owner() == owner) {
                result.entries_[result.count_++] = slot.descriptor;
            }
        }
    }
    ReleaseSRWLockShared(&g_registryLock);
    return result;
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_registryLock);
    g_slots = {};
    ReleaseSRWLockExclusive(&g_registryLock);
}

namespace {

// Separate from the registry lock: a slot calls into the registry while holding this one.
SRWLOCK g_slotLock{SRWLOCK_INIT};

} // namespace

bool PageRegistration::acquire(Owner owner,
                               std::string_view stableId,
                               std::string_view displayName,
                               FrameCallback callback,
                               void (*prepare)() noexcept) noexcept {
    AcquireSRWLockExclusive(&g_slotLock);
    if (registered_) {
        ReleaseSRWLockExclusive(&g_slotLock);
        return true;
    }
    if (prepare != nullptr) {
        prepare();
    }
    Descriptor descriptor;
    if (create_descriptor(owner, stableId, displayName, callback, descriptor)
        && register_module(descriptor)) {
        stableId_ = stableId;
        registered_ = true;
    }
    const bool registered = registered_;
    ReleaseSRWLockExclusive(&g_slotLock);
    return registered;
}

void PageRegistration::release(void (*finish)() noexcept) noexcept {
    AcquireSRWLockExclusive(&g_slotLock);
    if (registered_) {
        unregister_module(stableId_);
        stableId_ = {};
        registered_ = false;
    }
    if (finish != nullptr) {
        finish();
    }
    ReleaseSRWLockExclusive(&g_slotLock);
}

} // namespace sunrise::core::ui::modules::registry
