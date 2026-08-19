#include "viewer_panel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <imgui.h>

#include "../../../core/ui/components/label/ui_label_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../hooks/player_hold/player_hold.h"
#include "../../hooks/viewer_audio/viewer_audio.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../viewer/viewer_camera_settings_store.h"
#include "../components/key_picker/client_key_picker.h"

namespace sunrise::client::ui::viewer {
namespace {

namespace camera = client::viewer::camera;
namespace key_picker = client::ui::components::key_picker;
namespace label = core::ui::components::label;

void copy_pose(const camera::Pose& pose) noexcept {
    std::array<char, 256> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "position=(%.6f, %.6f, %.6f) "
                                      "forward=(%.6f, %.6f, %.6f) "
                                      "yaw=%.6f pitch=%.6f fov=%.3f",
                                      static_cast<double>(pose.position[0]),
                                      static_cast<double>(pose.position[1]),
                                      static_cast<double>(pose.position[2]),
                                      static_cast<double>(pose.forward[0]),
                                      static_cast<double>(pose.forward[1]),
                                      static_cast<double>(pose.forward[2]),
                                      static_cast<double>(pose.yaw),
                                      static_cast<double>(pose.pitch),
                                      static_cast<double>(pose.fov));
    if (written > 0) {
        ImGui::SetClipboardText(text.data());
    }
}

void draw_status(const camera::Status& status) noexcept {
    ImGui::TextUnformatted("Viewer Camera");
    ImGui::Separator();
    if (!status.installed) {
        ImGui::TextWrapped("Unavailable: %s", camera::failure_name(status.failure));
        return;
    }
    const char* state = status.active ? (status.applied ? "Active" : "Waiting for render")
                                      : (status.requested ? "Waiting for camera" : "Inactive");
    ImGui::Text("State: %s", state);
    ImGui::Text("Player hold: %s",
                hooks::player_hold::holding()
                    ? "Holding"
                    : (status.active ? "Waiting for physics" : "Inactive"));
    ImGui::Text(
        "Audio listener: %s",
        client::viewer::audio::applied()
            ? "Following camera"
            : (client::viewer::audio::installed() ? "Waiting for listener" : "Unavailable"));
    if (status.failure != camera::Failure::none) {
        ImGui::TextWrapped("Last issue: %s", camera::failure_name(status.failure));
    }
    ImGui::Text("Generation: %llu", static_cast<unsigned long long>(status.generation));
}

void draw_pose(const camera::Status& status) noexcept {
    ImGui::Spacing();
    ImGui::TextUnformatted("Pose");
    ImGui::Separator();
    ImGui::Text("Position  %.3f  %.3f  %.3f",
                static_cast<double>(status.pose.position[0]),
                static_cast<double>(status.pose.position[1]),
                static_cast<double>(status.pose.position[2]));
    ImGui::Text("Forward   %.3f  %.3f  %.3f",
                static_cast<double>(status.pose.forward[0]),
                static_cast<double>(status.pose.forward[1]),
                static_cast<double>(status.pose.forward[2]));
    ImGui::Text("FOV       %.2f", static_cast<double>(status.pose.fov));
    if (ImGui::Button("Copy pose")) {
        copy_pose(status.pose);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!status.active);
    if (ImGui::Button("Return to player")) {
        (void)camera::return_to_player();
    }
    ImGui::EndDisabled();
}

void draw_bookmarks(camera::Status status,
                    client::viewer::Settings& settings,
                    bool& changed) noexcept {
    ImGui::Spacing();
    ImGui::TextUnformatted("Bookmarks");
    ImGui::Separator();
    for (std::size_t index = 0; index < settings.bookmarks.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        client::viewer::Bookmark& bookmark = settings.bookmarks[index];
        ImGui::Text("%zu", index + 1);
        ImGui::SameLine();
        ImGui::BeginDisabled(!status.active);
        if (ImGui::Button("Save")) {
            bookmark.position = status.pose.position;
            bookmark.yaw = status.pose.yaw;
            bookmark.pitch = status.pose.pitch;
            bookmark.fov = settings.fov;
            bookmark.valid = true;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!status.active || !bookmark.valid);
        if (ImGui::Button("Go")) {
            camera::Pose destination = status.pose;
            destination.position = bookmark.position;
            destination.yaw = bookmark.yaw;
            destination.pitch = bookmark.pitch;
            destination.fov = bookmark.fov;
            if (camera::move_to(destination)) {
                settings.fov = bookmark.fov;
                changed = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!bookmark.valid);
        if (ImGui::Button("Clear")) {
            bookmark = {};
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

} // namespace

void draw() noexcept {
    const camera::Status status = camera::status();
    client::viewer::Settings settings = client::viewer::get();
    bool changed = false;

    draw_status(status);
    ImGui::Spacing();
    ImGui::BeginDisabled(!status.installed);
    if (ImGui::Button(status.requested ? "Exit Viewer" : "Enter Viewer")) {
        camera::request_active(!status.requested);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Movement bindings | Shift boost | Ctrl precision");

    ImGui::Spacing();
    const float labelWidth =
        label::inset() + ImGui::CalcTextSize("Precision").x + ImGui::GetStyle().ItemSpacing.x * 2;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine(labelWidth);
    changed = key_picker::control("viewer_toggle_key", settings.toggleKey, controlWidth) || changed;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Speed");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    changed = ImGui::SliderFloat("##viewer_speed",
                                 &settings.speed,
                                 client::viewer::kMinimumSpeed,
                                 client::viewer::kMaximumSpeed,
                                 "%.1f units/s")
              || changed;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Boost");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    changed = ImGui::SliderFloat("##viewer_boost",
                                 &settings.boostMultiplier,
                                 client::viewer::kMinimumBoostMultiplier,
                                 client::viewer::kMaximumBoostMultiplier,
                                 "%.2fx")
              || changed;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Precision");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    changed = ImGui::SliderFloat("##viewer_precision",
                                 &settings.precisionMultiplier,
                                 client::viewer::kMinimumPrecisionMultiplier,
                                 client::viewer::kMaximumPrecisionMultiplier,
                                 "%.2fx")
              || changed;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Mouse");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    changed = ImGui::SliderFloat("##viewer_mouse",
                                 &settings.mouseSensitivity,
                                 client::viewer::kMinimumMouseSensitivity,
                                 client::viewer::kMaximumMouseSensitivity,
                                 "%.4f")
              || changed;

    bool overrideFov = settings.fov != client::viewer::kNativeFov;
    if (core::ui::components::toggle::control("Override FOV##viewer", overrideFov)) {
        settings.fov = overrideFov ? std::clamp(status.pose.fov,
                                                client::viewer::kMinimumFov,
                                                client::viewer::kMaximumFov)
                                   : client::viewer::kNativeFov;
        changed = true;
    }
    if (overrideFov) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(1.0F, ImGui::GetContentRegionAvail().x));
        changed = ImGui::SliderFloat("##viewer_fov",
                                     &settings.fov,
                                     client::viewer::kMinimumFov,
                                     client::viewer::kMaximumFov,
                                     "%.1f degrees")
                  || changed;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Enter Viewer preset");
    ImGui::Separator();
    ImGui::TextWrapped("These only coordinate the independent Game presentation settings while "
                       "Viewer Camera is active.");
    changed = core::ui::components::toggle::control("Hide weapon##viewer_enter",
                                                    settings.hideWeaponOnEnter)
              || changed;
    changed =
        core::ui::components::toggle::control("Remove HUD##viewer_enter", settings.removeHudOnEnter)
        || changed;

    draw_pose(status);
    draw_bookmarks(status, settings, changed);

    if (changed && !client::viewer::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::viewer
