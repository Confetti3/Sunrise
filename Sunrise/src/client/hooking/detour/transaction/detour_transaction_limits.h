#pragma once

#include <cstddef>

namespace sunrise::client::hooking::detour::limits {

/** Fixed transaction storage covers the process thread walk without suspended-thread allocation. */
inline constexpr std::size_t kThreadCount = 1024;
/** Public batches admit at most this many attach or detach operations. */
inline constexpr std::size_t kOperationCount = 32;

} // namespace sunrise::client::hooking::detour::limits
