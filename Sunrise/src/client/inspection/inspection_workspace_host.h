#pragma once

#include "inspection_scene.h"

namespace sunrise::client::inspection::workspace_host {

/** Renderer-facing workspace boundary. No Inspector UI types cross this contract. */
void suspend() noexcept;
[[nodiscard]] bool visible() noexcept;
[[nodiscard]] bool render(bool uiVisible) noexcept;
void service_camera_path_captures() noexcept;

[[nodiscard]] bool depth_helpers_enabled() noexcept;
[[nodiscard]] SceneFramePtr scene_frame() noexcept;
void publish_depth_status(const DepthRenderStatus& status) noexcept;

} // namespace sunrise::client::inspection::workspace_host
