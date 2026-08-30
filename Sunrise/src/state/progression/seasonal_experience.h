#pragma once

#include <cstdint>
#include <span>

namespace sunrise::state::progression::seasonal_experience {

/** Resolves persistent storage and restores earned seasonal XP. */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Clears process memory without deleting persisted XP. */
void shutdown() noexcept;

/** Adds unmodified base XP to the account's seasonal progression. */
[[nodiscard]] bool grant(std::int32_t amount) noexcept;

/** Returns runtime-earned seasonal XP. */
[[nodiscard]] std::int32_t earned() noexcept;

/** Returns the one-based Season of Arrivals rank earned by the persisted XP total. */
[[nodiscard]] std::uint16_t rank() noexcept;

/** Returns whether one native Season of Arrivals reward row was already claimed. */
[[nodiscard]] bool reward_claimed(std::uint16_t rewardIndex) noexcept;

/** Persistently claims one native reward row exactly once. */
[[nodiscard]] bool claim_reward(std::uint16_t rewardIndex) noexcept;

/** Publishes persisted Season Pass claims into their mapped account acquired-flag bytes. */
[[nodiscard]] bool apply_reward_claims(std::span<std::uint8_t> acquiredFlags) noexcept;

} // namespace sunrise::state::progression::seasonal_experience
