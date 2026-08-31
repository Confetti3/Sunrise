#include "enemy_wave_queue.h"

#include <Windows.h>

#include <array>
#include <limits>

namespace sunrise::server::gameplay::mission {
namespace {

inline constexpr std::size_t kQueueCapacity = 32;

struct Slot final {
    QueuedEnemyWave value{};
    bool occupied{};
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Slot, kQueueCapacity> g_slots{};
std::uint64_t g_nextTicket = 1;

} // namespace

bool reserve_enemy_wave(std::uint64_t activitySessionId,
                        std::uint64_t hostGeneration,
                        std::uint64_t bindingGeneration,
                        const EnemyWaveIntent& intent,
                        EnemyWaveTicket& ticket) noexcept {
    ticket = {};
    if (activitySessionId == 0 || hostGeneration == 0 || bindingGeneration == 0
        || intent.commandId == 0
        || intent.waveId == 0 || intent.spawnerDefinition == 0 || intent.requestedCount == 0
        || intent.requestedCount > intent.requested.size()) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    Slot* free = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration
            && slot.value.intent.commandId == intent.commandId) {
            ticket = slot.value.ticket;
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
        if (!slot.occupied && free == nullptr) free = &slot;
    }
    if (free == nullptr || g_nextTicket == std::numeric_limits<std::uint64_t>::max()) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    free->value.ticket.value = g_nextTicket++;
    free->value.intent = intent;
    free->value.activitySessionId = activitySessionId;
    free->value.hostGeneration = hostGeneration;
    free->value.bindingGeneration = bindingGeneration;
    free->occupied = true;
    ticket = free->value.ticket;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool peek_enemy_wave(std::uint64_t activitySessionId,
                     std::uint64_t hostGeneration,
                     std::uint64_t bindingGeneration,
                     QueuedEnemyWave& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const Slot* selected = nullptr;
    for (const Slot& slot : g_slots) {
        if (slot.occupied && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration
            && (selected == nullptr || slot.value.ticket.value < selected->value.ticket.value)) {
            selected = &slot;
        }
    }
    if (selected != nullptr) output = selected->value;
    ReleaseSRWLockShared(&g_lock);
    return selected != nullptr;
}

bool mark_enemy_wave_published(EnemyWaveTicket ticket) noexcept {
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.ticket.value == ticket.value) {
            slot.value.published = true;
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

bool settle_enemy_wave(EnemyWaveTicket ticket) noexcept {
    if (ticket.value == 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.ticket.value == ticket.value && slot.value.published) {
            slot = {};
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

void cancel_enemy_waves(std::uint64_t activitySessionId,
                        std::uint64_t hostGeneration,
                        std::uint64_t bindingGeneration) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& slot : g_slots) {
        if (slot.occupied && slot.value.activitySessionId == activitySessionId
            && slot.value.hostGeneration == hostGeneration
            && slot.value.bindingGeneration == bindingGeneration) slot = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void reset_enemy_waves() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_slots = {};
    g_nextTicket = 1;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::gameplay::mission
