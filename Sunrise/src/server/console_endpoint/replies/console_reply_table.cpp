#include "console_reply_table.h"

#include <Windows.h>

#include <array>

namespace sunrise::server::console_endpoint::replies {
namespace {

struct Slot {
    std::uint64_t ticket{};
    core::console::Result result{};
};

std::array<Slot, kReplyCapacity> g_slots{};
std::size_t g_head{};
std::size_t g_count{};
SRWLOCK g_lock{SRWLOCK_INIT};

} // namespace

/** Stores one finished result under its ticket. */
bool remember(std::uint64_t ticket, const core::console::Result& result) noexcept {
    if (ticket == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_slots[(g_head + g_count) % kReplyCapacity] = Slot{ticket, result};
    if (g_count < kReplyCapacity) {
        ++g_count;
    } else {
        // Full: this write consumed the oldest, so the window slides rather than growing.
        g_head = (g_head + 1) % kReplyCapacity;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Takes one result back, removing it. */
bool take(std::uint64_t ticket, core::console::Result& output) noexcept {
    bool found = false;
    AcquireSRWLockExclusive(&g_lock);
    for (std::size_t offset = 0; offset < g_count; ++offset) {
        const std::size_t index = (g_head + offset) % kReplyCapacity;
        if (g_slots[index].ticket != ticket) {
            continue;
        }
        output = g_slots[index].result;
        // Compaction keeps the remaining order, which is what the burst was submitted in.
        for (std::size_t move = offset; move + 1 < g_count; ++move) {
            g_slots[(g_head + move) % kReplyCapacity] =
                g_slots[(g_head + move + 1) % kReplyCapacity];
        }
        --g_count;
        found = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

/** Drops every stored result. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_slots = {};
    g_head = 0;
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::server::console_endpoint::replies
