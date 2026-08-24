#include "inspection_settings_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"

namespace sunrise::client::inspection::settings {
namespace {

constexpr std::wstring_view kFileSuffix = L"\\inspector.json";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::size_t kFileCapacity = 4096;
constexpr std::size_t kScalarCapacity = 32;

enum class LoadResult : std::uint8_t {
    absent,
    loaded,
    invalid,
};

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
bool g_pathResolved{};

void report_fail(const char* reason) noexcept {
    std::array<char, 112> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=inspector stage=settings result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool valid(const Settings& settings) noexcept {
    return std::isfinite(settings.leftWidth) && settings.leftWidth >= kMinimumLeftWidth
           && settings.leftWidth <= kMaximumLeftWidth && std::isfinite(settings.rightWidth)
           && settings.rightWidth >= kMinimumRightWidth && settings.rightWidth <= kMaximumRightWidth
           && std::isfinite(settings.bottomHeight) && settings.bottomHeight >= kMinimumBottomHeight
           && settings.bottomHeight <= kMaximumBottomHeight && settings.overlayDetail <= 4
           && settings.maximumVisibleNodes >= 32 && settings.maximumVisibleNodes <= 1024
           && std::isfinite(settings.nearbyRadius) && settings.nearbyRadius >= 5.0F
           && settings.nearbyRadius <= 250.0F && std::isfinite(settings.glyphSizePixels)
           && settings.glyphSizePixels >= kMinimumGlyphSizePixels
           && settings.glyphSizePixels <= kMaximumGlyphSizePixels
           && std::isfinite(settings.lineWidthPixels) && settings.lineWidthPixels >= 1.0F
           && settings.lineWidthPixels <= 4.0F && std::isfinite(settings.baseOpacity)
           && settings.baseOpacity >= 0.15F && settings.baseOpacity <= 1.0F
           && std::isfinite(settings.focusContextOpacity) && settings.focusContextOpacity >= 0.05F
           && settings.focusContextOpacity <= 1.0F;
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
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
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

[[nodiscard]] bool
parse_float(std::string_view text, std::string_view key, float& output) noexcept {
    std::string_view scalar;
    std::array<char, kScalarCapacity> buffer{};
    if (!scalar_for(text, key, scalar) || !terminated(scalar, buffer)) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(buffer.data(), &end);
    if (errno != 0 || end == buffer.data() || *end != '\0') {
        return false;
    }
    output = parsed;
    return true;
}

[[nodiscard]] bool parse_bool(std::string_view text, std::string_view key, bool& output) noexcept {
    std::string_view scalar;
    if (!scalar_for(text, key, scalar)) {
        return false;
    }
    if (scalar == "true") {
        output = true;
        return true;
    }
    if (scalar == "false") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool
parse_uint32(std::string_view text, std::string_view key, std::uint32_t& output) noexcept {
    std::string_view scalar;
    std::array<char, kScalarCapacity> buffer{};
    if (!scalar_for(text, key, scalar) || !terminated(scalar, buffer)) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(buffer.data(), &end, 10);
    if (errno != 0 || end == buffer.data() || *end != '\0'
        || parsed > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool parse(std::string_view text, Settings& output) noexcept {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    if (begin == std::string_view::npos || text[begin] != '{' || text[end] != '}') {
        return false;
    }

    std::uint32_t schemaVersion = 0;
    std::uint32_t overlayDetail = 0;
    const bool complete =
        parse_uint32(text, "\"schema_version\"", schemaVersion) && schemaVersion == kSchemaVersion
        && parse_float(text, "\"left_width\"", output.leftWidth)
        && parse_float(text, "\"right_width\"", output.rightWidth)
        && parse_float(text, "\"bottom_height\"", output.bottomHeight)
        && parse_uint32(text, "\"overlay_detail\"", overlayDetail) && overlayDetail <= 4
        && parse_uint32(text, "\"maximum_visible_nodes\"", output.maximumVisibleNodes)
        && parse_float(text, "\"nearby_radius\"", output.nearbyRadius)
        && parse_float(text, "\"glyph_size_pixels\"", output.glyphSizePixels)
        && parse_float(text, "\"line_width_pixels\"", output.lineWidthPixels)
        && parse_float(text, "\"base_opacity\"", output.baseOpacity)
        && parse_float(text, "\"focus_context_opacity\"", output.focusContextOpacity)
        && parse_bool(text, "\"bottom_collapsed\"", output.bottomCollapsed)
        && parse_bool(text, "\"show_geometry\"", output.showGeometry)
        && parse_bool(text, "\"show_entities\"", output.showEntities)
        && parse_bool(text, "\"show_spawns\"", output.showSpawns)
        && parse_bool(text, "\"show_logic\"", output.showLogic)
        && parse_bool(text, "\"show_triggers\"", output.showTriggers)
        && parse_bool(text, "\"show_audio\"", output.showAudio)
        && parse_bool(text, "\"show_known_bounds\"", output.showKnownBounds)
        && parse_bool(text, "\"show_trigger_centers\"", output.showTriggerCenters)
        && parse_bool(text, "\"show_authored_orientation\"", output.showAuthoredOrientation)
        && parse_bool(text, "\"show_labels\"", output.showLabels);
    output.overlayDetail = static_cast<std::uint8_t>(overlayDetail);
    return complete && valid(output);
}

[[nodiscard]] LoadResult load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? LoadResult::absent : LoadResult::invalid;
    }
    std::array<char, kFileCapacity> buffer{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr)
        != FALSE;
    (void)CloseHandle(file);
    if (!readOk || read == 0 || read == static_cast<DWORD>(buffer.size() - 1)) {
        return LoadResult::invalid;
    }
    Settings parsed{};
    if (!parse(std::string_view(buffer.data(), read), parsed)) {
        return LoadResult::invalid;
    }
    g_settings = parsed;
    return LoadResult::loaded;
}

[[nodiscard]] bool store(const Settings& settings) noexcept {
    if (!g_pathResolved) {
        return false;
    }
    std::array<char, kFileCapacity> document{};
    const int used = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n"
                                   "  \"schema_version\": %u,\n"
                                   "  \"left_width\": %.2f,\n"
                                   "  \"right_width\": %.2f,\n"
                                   "  \"bottom_height\": %.2f,\n"
                                   "  \"overlay_detail\": %u,\n"
                                   "  \"maximum_visible_nodes\": %u,\n"
                                   "  \"nearby_radius\": %.2f,\n"
                                   "  \"glyph_size_pixels\": %.2f,\n"
                                   "  \"line_width_pixels\": %.2f,\n"
                                   "  \"base_opacity\": %.3f,\n"
                                   "  \"focus_context_opacity\": %.3f,\n"
                                   "  \"bottom_collapsed\": %s,\n"
                                   "  \"show_geometry\": %s,\n"
                                   "  \"show_entities\": %s,\n"
                                   "  \"show_spawns\": %s,\n"
                                   "  \"show_logic\": %s,\n"
                                   "  \"show_triggers\": %s,\n"
                                   "  \"show_audio\": %s,\n"
                                   "  \"show_known_bounds\": %s,\n"
                                   "  \"show_trigger_centers\": %s,\n"
                                   "  \"show_authored_orientation\": %s,\n"
                                   "  \"show_labels\": %s\n"
                                   "}\n",
                                   static_cast<unsigned>(kSchemaVersion),
                                   static_cast<double>(settings.leftWidth),
                                   static_cast<double>(settings.rightWidth),
                                   static_cast<double>(settings.bottomHeight),
                                   static_cast<unsigned>(settings.overlayDetail),
                                   static_cast<unsigned>(settings.maximumVisibleNodes),
                                   static_cast<double>(settings.nearbyRadius),
                                   static_cast<double>(settings.glyphSizePixels),
                                   static_cast<double>(settings.lineWidthPixels),
                                   static_cast<double>(settings.baseOpacity),
                                   static_cast<double>(settings.focusContextOpacity),
                                   settings.bottomCollapsed ? "true" : "false",
                                   settings.showGeometry ? "true" : "false",
                                   settings.showEntities ? "true" : "false",
                                   settings.showSpawns ? "true" : "false",
                                   settings.showLogic ? "true" : "false",
                                   settings.showTriggers ? "true" : "false",
                                   settings.showAudio ? "true" : "false",
                                   settings.showKnownBounds ? "true" : "false",
                                   settings.showTriggerCenters ? "true" : "false",
                                   settings.showAuthoredOrientation ? "true" : "false",
                                   settings.showLabels ? "true" : "false");
    if (used <= 0 || static_cast<std::size_t>(used) >= document.size()) {
        return false;
    }

    core::path::Buffer temporary = g_path;
    if (!core::path::append(temporary, L".tmp")) {
        return false;
    }
    const HANDLE file = CreateFileW(temporary.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(used), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(used) && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    if (complete) {
        complete = MoveFileExW(temporary.chars.data(),
                               g_path.chars.data(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   != FALSE;
    }
    if (!complete) {
        (void)DeleteFileW(temporary.chars.data());
    }
    return complete;
}

} // namespace

void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    if (!g_pathResolved) {
        report_fail("path");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const LoadResult loaded = load();
    if (loaded == LoadResult::absent) {
        if (!store(g_settings)) {
            report_fail("defaults-write");
        }
    } else if (loaded == LoadResult::invalid) {
        report_fail("parse-or-schema");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_path = {};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

Settings get() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Settings result = g_settings;
    ReleaseSRWLockShared(&g_lock);
    return result;
}

bool publish(const Settings& settings) noexcept {
    if (!valid(settings)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const bool stored = store(settings);
    if (stored) {
        g_settings = settings;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return stored;
}

} // namespace sunrise::client::inspection::settings
