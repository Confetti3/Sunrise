#pragma once

#include <cstdint>

namespace sunrise::server::script_host {

/**
 * Initializes the optional out-of-process script-host bridge.
 * A missing sidecar is not an error; service() retries the local pipe connection.
 */
[[nodiscard]] bool initialize() noexcept;

/** Runs one fixed-budget, nonblocking named-pipe service slice. */
void service(std::uint64_t now) noexcept;

/** Closes the pipe and clears bridge-owned scalar and buffer state. */
void shutdown() noexcept;

} // namespace sunrise::server::script_host
