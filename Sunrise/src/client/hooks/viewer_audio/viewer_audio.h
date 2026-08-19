#pragma once

namespace sunrise::client::viewer::audio {

/** Resolves and attaches the proven Wwise listener-position boundary. */
[[nodiscard]] bool install() noexcept;

/** Detaches the listener boundary after all replacement calls drain. */
[[nodiscard]] bool uninstall() noexcept;

/** Publishes Viewer pose, or a one-shot native restoration, on the camera frame. */
void apply() noexcept;

/** @return True while the listener boundary is attached. */
[[nodiscard]] bool installed() noexcept;

/** @return True after the latest listener-zero update used Viewer pose. */
[[nodiscard]] bool applied() noexcept;

} // namespace sunrise::client::viewer::audio
