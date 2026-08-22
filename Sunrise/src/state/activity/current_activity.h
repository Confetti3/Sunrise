#pragma once

#include <cstdint>

namespace sunrise::state::activity {

/**
 * The activity the account is currently in.
 *
 * A collectible pickup arrives as an incident, and which book it should fill depends on where the
 * player is. The incident does carry the bubble, but only in a position that differs per incident
 * type, and there are several: a vase reports type 2 and a dead ghost type 10, each with its own
 * schema. The selection that put the player there already names the bubble once, so keeping it is
 * cheaper than decoding every type's payload and works for types not yet seen.
 */

/** No activity has been selected yet. */
inline constexpr std::uint32_t kNoBubble = 0;

/**
 * Records the arrival bubble of the activity just selected.
 * @param bubble Arrival bubble hash, or kNoBubble when the selection carried none.
 */
void set_current_bubble(std::uint32_t bubble) noexcept;

/** @return The arrival bubble of the current activity, or kNoBubble. */
[[nodiscard]] std::uint32_t current_bubble() noexcept;

} // namespace sunrise::state::activity
