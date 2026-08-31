#pragma once

#include <cstdint>

namespace sunrise::server::console_endpoint {

/**
 * Binds the loopback console endpoint when the settings enable it.
 * @return True when the endpoint is bound, or when it is disabled and nothing was bound.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Runs one bounded endpoint slice on the caller thread.
 *
 * Accepts at most one connection, reads whole lines, submits them, and writes back whatever the
 * drain has finished. No handler runs here: this thread only ever queues and reports.
 *
 * @param now Monotonic tick count.
 */
void service(std::uint64_t now) noexcept;

/** Closes the endpoint's sockets. */
void shutdown() noexcept;

} // namespace sunrise::server::console_endpoint
