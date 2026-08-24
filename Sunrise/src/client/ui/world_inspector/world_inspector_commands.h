#pragma once

#include <cstdint>
#include <string_view>

#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::commands {

void copy_text(std::string_view text) noexcept;
void copy_id(inspection::NodeId id) noexcept;
void copy_tag(std::uint32_t tag) noexcept;
void copy_position(const inspection::Transform& transform) noexcept;
void copy_camera_position(const client::viewer::camera::Pose& pose) noexcept;

} // namespace sunrise::client::ui::world_inspector::commands
