#include "world_inspector_commands.h"

#include <array>
#include <cstdio>

#include "../../../core/ui/modules/logs/logs.h"

namespace sunrise::client::ui::world_inspector::commands {

void copy_text(std::string_view text) noexcept {
    if (!text.empty()) {
        (void)core::ui::modules::logs::queue_text_copy(text);
    }
}

void copy_id(inspection::NodeId id) noexcept {
    std::array<char, 32> text{};
    const int written = std::snprintf(
        text.data(), text.size(), "0x%016llX", static_cast<unsigned long long>(id.value));
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        copy_text(std::string_view(text.data(), static_cast<std::size_t>(written)));
    }
}

void copy_tag(std::uint32_t tag) noexcept {
    std::array<char, 16> text{};
    const int written = std::snprintf(text.data(), text.size(), "0x%08X", tag);
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        copy_text(std::string_view(text.data(), static_cast<std::size_t>(written)));
    }
}

void copy_position(const inspection::Transform& transform) noexcept {
    std::array<char, 128> text{};
    const auto& position = transform.position;
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "%.6f, %.6f, %.6f",
                                      static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2]));
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        copy_text(std::string_view(text.data(), static_cast<std::size_t>(written)));
    }
}

void copy_camera_position(const client::viewer::camera::Pose& pose) noexcept {
    inspection::Transform transform{};
    transform.position = pose.position;
    copy_position(transform);
}

} // namespace sunrise::client::ui::world_inspector::commands
