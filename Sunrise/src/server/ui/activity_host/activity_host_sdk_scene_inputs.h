#pragma once

#include <cstdint>

#include "../../activity/activity_sdk_mission_runtime.h"

namespace sunrise::server::ui::activity_host::sdk_mission_view {

/**
 * Draws a scene's Inputs button and its popup of input buttons and outputs.
 * @param available False disables the input buttons.
 * @param sent Set when an input was sent this frame.
 * @return The send's status; ready when nothing was sent.
 */
[[nodiscard]] server::activity::activity_sdk_mission::SceneStatus
draw_scene_inputs(const state::activity_sdk::BoundView& view,
                  std::uint32_t occurrenceRow,
                  std::uint32_t slotRow,
                  bool available,
                  bool& sent) noexcept;

} // namespace sunrise::server::ui::activity_host::sdk_mission_view
