#pragma once

namespace sunrise::middleware {

/** Loads the named package catalog on first boot when State has no complete cache. */
[[nodiscard]] bool initialize() noexcept;

/** Releases nothing; State owns the generated content cache. */
void shutdown() noexcept;

} // namespace sunrise::middleware
