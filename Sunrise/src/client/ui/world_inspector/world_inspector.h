#pragma once

#include <cstdint>

namespace sunrise::client::ui::world_inspector {

struct SelectionIdentity final {
    std::uint64_t producerEpoch{};
    std::uint64_t nativeKey{};
    std::uint32_t producer{};
    std::uint32_t kind{};
};

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

/** Copies the current graph-independent selection identity. */
[[nodiscard]] bool selected_identity(SelectionIdentity& identity) noexcept;

/** Services queued labeled-keyframe captures on the render thread, even when Inspector is closed. */
void service_camera_path_captures() noexcept;

} // namespace sunrise::client::ui::world_inspector
