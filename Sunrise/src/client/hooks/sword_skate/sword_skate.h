#pragma once

namespace sunrise::client::hooks::sword_skate {

/**
 * Clears the glide refusal for one physics tick of the local player.
 * Call it from the physics sync: the flag is written and read inside the tick the jump is pressed.
 *
 * @param component Physics component being synced, tested for player ownership here.
 */
void apply(void* component) noexcept;

} // namespace sunrise::client::hooks::sword_skate
