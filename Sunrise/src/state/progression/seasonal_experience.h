#pragma once

#include <cstdint>

namespace sunrise::state::progression::seasonal_experience {

/** Resolves persistent storage and restores earned seasonal XP. */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Clears process memory without deleting persisted XP. */
void shutdown() noexcept;

/** Adds unmodified base XP to the account's seasonal progression. */
[[nodiscard]] bool grant(std::int32_t amount) noexcept;

/** Returns runtime-earned seasonal XP. */
[[nodiscard]] std::int32_t earned() noexcept;

} // namespace sunrise::state::progression::seasonal_experience
