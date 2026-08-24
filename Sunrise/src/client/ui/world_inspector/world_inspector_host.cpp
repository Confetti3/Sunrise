#include "../../inspection/inspection_workspace_host.h"
#include "world_inspector.h"

namespace sunrise::client::inspection::workspace_host {

void suspend() noexcept {
    ui::world_inspector::suspend();
}

bool visible() noexcept {
    return ui::world_inspector::visible();
}

bool render(bool uiVisible) noexcept {
    return ui::world_inspector::render(uiVisible);
}

void service_camera_path_captures() noexcept {
    ui::world_inspector::service_camera_path_captures();
}

bool depth_helpers_enabled() noexcept {
    return ui::world_inspector::debug_scene::enabled();
}

SceneFramePtr scene_frame() noexcept {
    return ui::world_inspector::debug_scene::frame();
}

void publish_depth_status(const DepthRenderStatus& status) noexcept {
    ui::world_inspector::debug_scene::publish_status(status);
}

} // namespace sunrise::client::inspection::workspace_host
