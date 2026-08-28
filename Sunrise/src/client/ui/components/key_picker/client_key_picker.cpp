#include "client_key_picker.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <imgui.h>

namespace sunrise::client::ui::components::key_picker {
namespace {

constexpr std::size_t kKeyNameCapacity = 64;

ImGuiID g_capturing{};

void key_name(std::uint32_t virtualKey, std::array<char, kKeyNameCapacity>& output) noexcept {
    if (virtualKey == 0) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, kKeyNameCapacity> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

[[nodiscard]] bool capture_key(std::uint32_t& picked) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        picked = 0;
        return true;
    }
    for (int key = VK_XBUTTON2 + 1; key <= VK_OEM_CLEAR; ++key) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            picked = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

} // namespace

bool control(const char* id, std::uint32_t& virtualKey, float width) noexcept {
    ImGui::PushID(id);
    const ImGuiID picker = ImGui::GetID("key_picker");
    if (g_capturing == picker) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturing = 0;
        }
        ImGui::PopID();
        std::uint32_t picked = 0;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturing = 0;
            return true;
        }
        return false;
    }
    std::array<char, kKeyNameCapacity> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturing = picker;
    }
    return false;
}

} // namespace sunrise::client::ui::components::key_picker
