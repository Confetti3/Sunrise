#pragma once

#include <algorithm>
#include <imgui.h>

#include "../../../core/ui/scaling/dpi/ui_dpi_scaling.h"

namespace sunrise::server::ui::activity_host::tool_window {

/** A titled window that moves, resizes and scrolls its own content. */
constexpr ImGuiWindowFlags kWindowFlags =
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

/** Applies bounded size constraints and a centered first-use size given as work-area fractions. */
inline void set_next(const ImVec2& workFraction) noexcept {
    namespace scaling = core::ui::scaling::dpi;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return;
    }
    const float margin = scaling::pixels(16.0F);
    const ImVec2 maximum{(std::max)(viewport->WorkSize.x - (margin * 2.0F), 1.0F),
                         (std::max)(viewport->WorkSize.y - (margin * 2.0F), 1.0F)};
    const ImVec2 requestedMinimum = scaling::pixels({320.0F, 120.0F});
    const ImVec2 minimum{(std::min)(requestedMinimum.x, maximum.x),
                         (std::min)(requestedMinimum.y, maximum.y)};
    const ImVec2 size{std::clamp(viewport->WorkSize.x * workFraction.x, minimum.x, maximum.x),
                      std::clamp(viewport->WorkSize.y * workFraction.y, minimum.y, maximum.y)};
    ImGui::SetNextWindowSizeConstraints(minimum, maximum);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, {0.5F, 0.5F});
}

/** Begins a movable window that resizes from its edges, with a title bar in the panel colour. */
[[nodiscard]] inline bool begin(const char* id, bool& open, const ImVec2& workFraction) noexcept {
    set_next(workFraction);
    // Dear ImGui reads these only while Begin draws the frame, so the body keeps the theme.
    // The same colour whether focused or not, so focus never lights the bar up. The corner grips
    // are drawn transparent; the edges carry the resize.
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 titleBackground = style.Colors[ImGuiCol_ChildBg];
    // Alpha 0 hides the corner grips; the edges still resize.
    constexpr ImVec4 kTransparent{0.0F, 0.0F, 0.0F, 0.0F};
    ImGui::PushStyleColor(ImGuiCol_TitleBg, titleBackground);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, titleBackground);
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, kTransparent);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, kTransparent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, style.WindowBorderSize);
    const bool visible = ImGui::Begin(id, &open, kWindowFlags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return visible;
}

} // namespace sunrise::server::ui::activity_host::tool_window
