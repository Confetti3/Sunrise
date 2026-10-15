#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "format.h"

namespace sunrise::state::activity_sdk {

/** @return One scene's gate rows; the section is sorted by scene slot row. */
[[nodiscard]] std::span<const format::AuthoredSceneEventKey>
scene_gates(std::span<const format::AuthoredSceneEventKey> gates, std::uint32_t slotRow) noexcept;

/** @return True when a graph block of the scene waits on the key, so sending it can act. */
[[nodiscard]] bool scene_listens(std::span<const format::AuthoredSceneEventKey> gates,
                                 std::uint32_t slotRow,
                                 std::uint32_t key) noexcept;

/** @return The known trigger name for a scene event key, or empty. Names match by FNV-1 hash. */
[[nodiscard]] std::string_view scene_trigger_name(std::uint32_t key) noexcept;

/** @return True for an input: graph blocks wait on it and none fires it, so a script sends it. */
[[nodiscard]] bool scene_trigger_is_input(const format::AuthoredSceneEventKey& gate) noexcept;

/** @return True for an output: a graph block fires it and none waits on it. */
[[nodiscard]] bool scene_trigger_is_output(const format::AuthoredSceneEventKey& gate) noexcept;

} // namespace sunrise::state::activity_sdk
