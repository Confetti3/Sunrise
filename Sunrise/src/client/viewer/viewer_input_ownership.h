#pragma once

namespace sunrise::client::viewer::input {

/** Publishes whether the inspector viewport currently owns Viewer Camera navigation. */
void set_workspace_navigation(bool active) noexcept;

/** @return True only while the visible inspector viewport owns camera navigation. */
[[nodiscard]] bool workspace_navigation() noexcept;

/** Clears any viewport input claim during close, focus loss, or shutdown. */
void reset() noexcept;

} // namespace sunrise::client::viewer::input
