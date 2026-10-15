#pragma once

#include <cstdint>

namespace sunrise::server::studio {

/**
 * Answers one Studio command per request id on the server thread. Studio creates the mapping;
 * without it this only looks for one once a second.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/** Unmaps the command mapping. */
void shutdown() noexcept;

} // namespace sunrise::server::studio
