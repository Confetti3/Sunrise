#include "current_activity.h"

#include <atomic>

namespace sunrise::state::activity {
namespace {

std::atomic<std::uint32_t> g_bubble{kNoBubble};

} // namespace

/** Records the arrival bubble of the activity just selected. */
void set_current_bubble(std::uint32_t bubble) noexcept {
    g_bubble.store(bubble, std::memory_order_relaxed);
}

/** @return The arrival bubble of the current activity. */
std::uint32_t current_bubble() noexcept {
    return g_bubble.load(std::memory_order_relaxed);
}

} // namespace sunrise::state::activity
