#include "viewer_camera_settings_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::viewer {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\viewer.json";
constexpr std::size_t kFileCapacity = 4096;
constexpr std::size_t kScalarCapacity = 32;
constexpr std::uint32_t kMaximumVirtualKey = 254;

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
bool g_pathResolved{};

[[nodiscard]] bool valid_fov(float value) noexcept {
    return value == kNativeFov || (value >= kMinimumFov && value <= kMaximumFov);
}

[[nodiscard]] bool valid(const Settings& settings) noexcept {
    if (settings.toggleKey > kMaximumVirtualKey || !std::isfinite(settings.speed)
        || settings.speed < kMinimumSpeed || settings.speed > kMaximumSpeed
        || !std::isfinite(settings.boostMultiplier)
        || settings.boostMultiplier < kMinimumBoostMultiplier
        || settings.boostMultiplier > kMaximumBoostMultiplier
        || !std::isfinite(settings.precisionMultiplier)
        || settings.precisionMultiplier < kMinimumPrecisionMultiplier
        || settings.precisionMultiplier > kMaximumPrecisionMultiplier
        || !std::isfinite(settings.mouseSensitivity)
        || settings.mouseSensitivity < kMinimumMouseSensitivity
        || settings.mouseSensitivity > kMaximumMouseSensitivity || !std::isfinite(settings.fov)
        || !valid_fov(settings.fov) || !std::isfinite(settings.inspectorLeftWidth)
        || settings.inspectorLeftWidth < kMinimumInspectorLeftWidth
        || settings.inspectorLeftWidth > kMaximumInspectorLeftWidth
        || !std::isfinite(settings.inspectorRightWidth)
        || settings.inspectorRightWidth < kMinimumInspectorRightWidth
        || settings.inspectorRightWidth > kMaximumInspectorRightWidth
        || !std::isfinite(settings.inspectorBottomHeight)
        || settings.inspectorBottomHeight < kMinimumInspectorBottomHeight
        || settings.inspectorBottomHeight > kMaximumInspectorBottomHeight) {
        return false;
    }
    for (const Bookmark& bookmark : settings.bookmarks) {
        if (!bookmark.valid) {
            continue;
        }
        if (!std::isfinite(bookmark.yaw) || !std::isfinite(bookmark.pitch)
            || !std::isfinite(bookmark.fov) || !valid_fov(bookmark.fov)
            || std::ranges::any_of(bookmark.position,
                                   [](float value) { return !std::isfinite(value); })) {
            return false;
        }
    }
    return true;
}

void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=viewer stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool
scalar_for(std::string_view text, std::string_view key, std::string_view& output) noexcept {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return false;
    }
    std::size_t begin = colon + 1;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = begin;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n'
           && text[end] != '\r') {
        ++end;
    }
    output = text.substr(begin, end - begin);
    return !output.empty();
}

[[nodiscard]] bool terminated(std::string_view value,
                              std::array<char, kScalarCapacity>& output) noexcept {
    if (value.size() >= output.size()) {
        return false;
    }
    std::ranges::copy(value, output.begin());
    output[value.size()] = '\0';
    return true;
}

void parse_float(std::string_view text, std::string_view key, float& output) noexcept {
    std::string_view scalar;
    std::array<char, kScalarCapacity> buffer{};
    if (scalar_for(text, key, scalar) && terminated(scalar, buffer)) {
        output = std::strtof(buffer.data(), nullptr);
    }
}

void parse_bool(std::string_view text, std::string_view key, bool& output) noexcept {
    std::string_view scalar;
    if (scalar_for(text, key, scalar)) {
        output = scalar.starts_with("true");
    }
}

void parse(std::string_view text, Settings& output) noexcept {
    std::string_view scalar;
    std::array<char, kScalarCapacity> buffer{};
    if (scalar_for(text, "\"toggle_key\"", scalar) && terminated(scalar, buffer)) {
        output.toggleKey = static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
    parse_float(text, "\"speed\"", output.speed);
    parse_float(text, "\"boost_multiplier\"", output.boostMultiplier);
    parse_float(text, "\"precision_multiplier\"", output.precisionMultiplier);
    parse_float(text, "\"mouse_sensitivity\"", output.mouseSensitivity);
    parse_float(text, "\"fov\"", output.fov);
    parse_float(text, "\"inspector_left_width\"", output.inspectorLeftWidth);
    parse_float(text, "\"inspector_right_width\"", output.inspectorRightWidth);
    parse_float(text, "\"inspector_bottom_height\"", output.inspectorBottomHeight);
    parse_bool(text, "\"hide_weapon_on_enter\"", output.hideWeaponOnEnter);
    parse_bool(text, "\"remove_hud_on_enter\"", output.removeHudOnEnter);
    parse_bool(text, "\"inspector_bottom_collapsed\"", output.inspectorBottomCollapsed);

    for (std::size_t index = 0; index < output.bookmarks.size(); ++index) {
        Bookmark& bookmark = output.bookmarks[index];
        std::array<char, 48> key{};
        (void)std::snprintf(key.data(), key.size(), "\"bookmark_%zu_valid\"", index);
        parse_bool(text, key.data(), bookmark.valid);
        for (std::size_t lane = 0; lane < bookmark.position.size(); ++lane) {
            (void)std::snprintf(
                key.data(), key.size(), "\"bookmark_%zu_position_%zu\"", index, lane);
            parse_float(text, key.data(), bookmark.position[lane]);
        }
        (void)std::snprintf(key.data(), key.size(), "\"bookmark_%zu_yaw\"", index);
        parse_float(text, key.data(), bookmark.yaw);
        (void)std::snprintf(key.data(), key.size(), "\"bookmark_%zu_pitch\"", index);
        parse_float(text, key.data(), bookmark.pitch);
        (void)std::snprintf(key.data(), key.size(), "\"bookmark_%zu_fov\"", index);
        parse_float(text, key.data(), bookmark.fov);
    }
}

[[nodiscard]] bool store(const Settings& settings) noexcept {
    if (!g_pathResolved) {
        return false;
    }
    std::array<char, kFileCapacity> document{};
    std::size_t used = 0;
    const auto append = [&document, &used](const char* format, auto... values) noexcept {
        if (used >= document.size()) {
            return false;
        }
        const int written =
            std::snprintf(document.data() + used, document.size() - used, format, values...);
        if (written <= 0 || static_cast<std::size_t>(written) >= document.size() - used) {
            return false;
        }
        used += static_cast<std::size_t>(written);
        return true;
    };

    bool complete = append("{\n  \"toggle_key\": %u,\n  \"speed\": %.4f,\n"
                           "  \"boost_multiplier\": %.4f,\n"
                           "  \"precision_multiplier\": %.4f,\n"
                           "  \"mouse_sensitivity\": %.6f,\n  \"fov\": %.4f,\n"
                           "  \"inspector_left_width\": %.2f,\n"
                           "  \"inspector_right_width\": %.2f,\n"
                           "  \"inspector_bottom_height\": %.2f,\n"
                           "  \"hide_weapon_on_enter\": %s,\n"
                           "  \"remove_hud_on_enter\": %s,\n"
                           "  \"inspector_bottom_collapsed\": %s",
                           static_cast<unsigned>(settings.toggleKey),
                           static_cast<double>(settings.speed),
                           static_cast<double>(settings.boostMultiplier),
                           static_cast<double>(settings.precisionMultiplier),
                           static_cast<double>(settings.mouseSensitivity),
                           static_cast<double>(settings.fov),
                           static_cast<double>(settings.inspectorLeftWidth),
                           static_cast<double>(settings.inspectorRightWidth),
                           static_cast<double>(settings.inspectorBottomHeight),
                           settings.hideWeaponOnEnter ? "true" : "false",
                           settings.removeHudOnEnter ? "true" : "false",
                           settings.inspectorBottomCollapsed ? "true" : "false");
    for (std::size_t index = 0; complete && index < settings.bookmarks.size(); ++index) {
        const Bookmark& bookmark = settings.bookmarks[index];
        complete = append(",\n  \"bookmark_%zu_valid\": %s,\n"
                          "  \"bookmark_%zu_position_0\": %.6f,\n"
                          "  \"bookmark_%zu_position_1\": %.6f,\n"
                          "  \"bookmark_%zu_position_2\": %.6f,\n"
                          "  \"bookmark_%zu_yaw\": %.6f,\n"
                          "  \"bookmark_%zu_pitch\": %.6f,\n"
                          "  \"bookmark_%zu_fov\": %.4f",
                          index,
                          bookmark.valid ? "true" : "false",
                          index,
                          static_cast<double>(bookmark.position[0]),
                          index,
                          static_cast<double>(bookmark.position[1]),
                          index,
                          static_cast<double>(bookmark.position[2]),
                          index,
                          static_cast<double>(bookmark.yaw),
                          index,
                          static_cast<double>(bookmark.pitch),
                          index,
                          static_cast<double>(bookmark.fov));
    }
    complete = complete && append("\n}\n");
    if (!complete) {
        return false;
    }

    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    complete =
        WriteFile(file, document.data(), static_cast<DWORD>(used), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(used);
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

void load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::array<char, kFileCapacity> buffer{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (!readOk || read == 0) {
        return;
    }
    Settings parsed{};
    parse(std::string_view(buffer.data(), read), parsed);
    if (!valid(parsed)) {
        report_fail("range");
        return;
    }
    g_settings = parsed;
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_path = core::path::Buffer{};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

Settings get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Settings snapshot = g_settings;
    ReleaseSRWLockShared(&g_lock);
    return snapshot;
}

bool publish(const Settings& settings) noexcept {
    if (!valid(settings)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings = settings;
    const bool stored = store(settings);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return true;
}

} // namespace sunrise::client::viewer
