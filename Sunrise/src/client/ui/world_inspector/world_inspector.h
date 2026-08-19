#pragma once

namespace sunrise::client::ui::world_inspector {

/** Initializes render-thread workspace state. */
void initialize() noexcept;

/** Clears workspace, provider, and input ownership state. */
void shutdown() noexcept;

/** Opens the full-screen workspace on the next UI frame. */
void open() noexcept;

/** Returns to the ordinary Sunrise surface. */
void close() noexcept;

/** Clears transient viewport input while the output surface is unavailable. */
void suspend() noexcept;

/** @return True while the full-screen workspace is selected. */
[[nodiscard]] bool visible() noexcept;

/** Draws the workspace when open and the global UI is visible. */
[[nodiscard]] bool render(bool uiVisible) noexcept;

} // namespace sunrise::client::ui::world_inspector
