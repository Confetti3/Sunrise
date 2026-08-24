#pragma once

#include <array>

namespace sunrise::client::viewer::audio {

/** Primary listener position copied into Destiny world-axis order. */
struct ListenerSnapshot final {
    std::array<float, 3> position{};
    bool viewerOverride{};
};

/**
 * Resolves and attaches the proven Wwise listener-position boundary.
 * @return True when the
 * listener replacement is attached.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Detaches the listener boundary after all replacement calls drain.
 * @return True when the
 * replacement drained and detached.
 */
[[nodiscard]] bool uninstall() noexcept;

/** Publishes Viewer pose, or a one-shot native restoration, on the camera frame. */
void apply() noexcept;

/** @return True while the listener boundary is attached. */
[[nodiscard]] bool installed() noexcept;

/** @return True after the latest listener-zero update used Viewer pose. */
[[nodiscard]] bool applied() noexcept;

/**
 * Copies listener zero's effective position without exposing Wwise-owned memory.
 * @param
 * output Destination for the copied listener state.
 * @return True when a native or Viewer
 * listener position has been observed.
 */
[[nodiscard]] bool listener_snapshot(ListenerSnapshot& output) noexcept;

} // namespace sunrise::client::viewer::audio
