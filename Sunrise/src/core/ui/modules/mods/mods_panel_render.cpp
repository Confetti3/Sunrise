/** The Mods page: every mod folder, loaded mods first in load order, then the refused ones. */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <span>
#include <string_view>

#include "../../../../state/mods/config/mod_config.h"
#include "../../../../state/mods/manifest/mod_version.h"
#include "../../../../state/mods/mods_runtime.h"
#include "../../components/section/ui_section_component.h"
#include "internal.h"

namespace sunrise::core::ui::modules::mods::internal {
namespace {

using state::mods::ConfigEntry;
using state::mods::ConfigType;
using state::mods::ConfigValue;
using state::mods::Record;
using state::mods::Severity;

/** One header label: name, version, status and a stable id suffix. */
constexpr std::size_t kLabelCapacity = 192;
/** One line naming a dependency and its version range. */
constexpr std::size_t kLineCapacity = 192;
/** One count with its noun, such as `12 package files`. */
constexpr std::size_t kCountCapacity = 32;
/** The mods folder path, converted to UTF-8 for display. */
constexpr std::size_t kFolderTextCapacity = 1024;
/** Wrap at the right edge of the content region. */
constexpr float kWrapAtRegionEdge = 0.0F;
/** Width of one number field in font heights, so it follows the interface scale. */
constexpr float kFieldWidthEms = 8.0F;
/** Nine significant digits preserve a float when the control commits it. */
constexpr char kFloatFormat[] = "%.9g";
/** A number field has no step buttons. */
constexpr int kNoIntegerStep = 0;
constexpr float kNoFloatStep = 0.0F;

/** What to do with one config value once the mod list is released. */
enum class EditKind : std::uint8_t {
    none,
    write,
    reset,
};

/** One finished config change. It is applied after the visit, because the store visits too. */
struct Edit final {
    EditKind kind{EditKind::none};
    state::mods::Id modId{};
    state::mods::Text<state::mods::kConfigKeyCapacity> key{};
    ConfigValue value{};
};

/** Draws text that wraps at the region edge. It need not be null-terminated. */
void wrapped(std::string_view text) noexcept {
    ImGui::PushTextWrapPos(kWrapAtRegionEdge);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

/** Draws `name value` on one row, with the name muted. Empty values draw nothing. */
void field(const char* name, std::string_view value) noexcept {
    if (value.empty()) {
        return;
    }
    ImGui::TextDisabled("%s", name);
    ImGui::SameLine();
    wrapped(value);
}

/** Draws the authors as one comma-separated row. */
void draw_authors(const state::mods::Manifest& manifest) noexcept {
    if (manifest.authorCount == 0) {
        return;
    }
    ImGui::TextDisabled("By");
    for (std::size_t index = 0; index < manifest.authorCount; ++index) {
        ImGui::SameLine();
        const std::string_view author = manifest.authors[index].view();
        const bool last = index + 1 == manifest.authorCount;
        ImGui::Text("%.*s%s", static_cast<int>(author.size()), author.data(), last ? "" : ",");
    }
}

/** Draws each dependency with its range and each incompatible id. */
void draw_relations(const state::mods::Manifest& manifest) noexcept {
    for (std::size_t index = 0; index < manifest.dependencyCount; ++index) {
        const state::mods::Dependency& dependency = manifest.dependencies[index];
        std::array<char, kLineCapacity> line{};
        const auto minimum = state::mods::version::text(dependency.minimum);
        const auto maximum = state::mods::version::text(dependency.maximum);
        if (dependency.hasMaximum) {
            (void)std::snprintf(line.data(),
                                line.size(),
                                "%s %s to %s",
                                dependency.id.bytes.data(),
                                minimum.data(),
                                maximum.data());
        } else {
            (void)std::snprintf(line.data(),
                                line.size(),
                                "%s %s or newer",
                                dependency.id.bytes.data(),
                                minimum.data());
        }
        field("Needs", line.data());
    }
    for (std::size_t index = 0; index < manifest.incompatibleCount; ++index) {
        field("Incompatible with", manifest.incompatible[index].view());
    }
}

/** Draws what a loaded mod adds to the game, naming only the parts it has. */
void draw_contents(const Record& record) noexcept {
    std::array<char, kCountCapacity> files{};
    std::array<char, kCountCapacity> patches{};
    if (record.newPackageFiles != 0) {
        (void)std::snprintf(files.data(),
                            files.size(),
                            "%u package file%s",
                            record.newPackageFiles,
                            record.newPackageFiles == 1 ? "" : "s");
    }
    if (record.patchFiles != 0) {
        (void)std::snprintf(patches.data(),
                            patches.size(),
                            "%u patch%s",
                            record.patchFiles,
                            record.patchFiles == 1 ? "" : "es");
    }
    const std::array parts{std::string_view(files.data()),
                           std::string_view(patches.data()),
                           std::string_view(record.hasSdk ? "mission SDK" : ""),
                           std::string_view(record.hasScripts ? "mission scripts" : "")};
    std::array<char, kLineCapacity> line{};
    std::size_t used = 0;
    for (const std::string_view part : parts) {
        const std::string_view separator = used == 0 ? "" : ", ";
        if (part.empty() || used + separator.size() + part.size() >= line.size()) {
            continue;
        }
        char* out = std::copy(separator.begin(), separator.end(), line.data() + used);
        out = std::copy(part.begin(), part.end(), out);
        used = static_cast<std::size_t>(out - line.data());
    }
    field("Adds", {line.data(), used});
}

/** Draws refusals in the accent color and warnings muted. */
void draw_problems(const Record& record) noexcept {
    const ImVec4 refusalColor = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    for (std::size_t index = 0; index < record.problemCount; ++index) {
        const state::mods::Problem& problem = record.problems[index];
        const bool refusal = problem.severity == Severity::refusal;
        ImGui::PushStyleColor(ImGuiCol_Text,
                              refusal ? refusalColor
                                      : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(refusal ? "Not loaded:" : "Warning:");
        ImGui::SameLine();
        wrapped(problem.message.view());
        ImGui::PopStyleColor();
    }
    if (record.droppedProblems != 0) {
        ImGui::TextDisabled("%zu additional problems were not retained.", record.droppedProblems);
    }
}

/**
 * Draws the control for one value. A number commits when its field loses focus, not per key.
 * @param value Current value; receives the edit.
 * @return True when an edit is finished.
 */
[[nodiscard]] bool draw_value(ConfigValue& value) noexcept {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * kFieldWidthEms);
    switch (value.type) {
    case ConfigType::boolean:
        return ImGui::Checkbox("##value", &value.boolean);
    case ConfigType::integer:
        (void)ImGui::InputInt("##value", &value.integer, kNoIntegerStep, kNoIntegerStep);
        return ImGui::IsItemDeactivatedAfterEdit();
    case ConfigType::floating:
        (void)ImGui::InputFloat(
            "##value", &value.floating, kNoFloatStep, kNoFloatStep, kFloatFormat);
        return ImGui::IsItemDeactivatedAfterEdit();
    }
    return false;
}

/** Shows the key and its default when the label is hovered. */
void default_tooltip(const ConfigEntry& entry) noexcept {
    const ConfigValue& fallback = entry.defaultValue;
    const char* key = entry.key.bytes.data();
    switch (fallback.type) {
    case ConfigType::integer:
        ImGui::SetItemTooltip("%s, default %d", key, fallback.integer);
        break;
    case ConfigType::floating:
        ImGui::SetItemTooltip("%s, default %.9g", key, static_cast<double>(fallback.floating));
        break;
    case ConfigType::boolean:
        ImGui::SetItemTooltip("%s, default %s", key, fallback.boolean ? "on" : "off");
        break;
    }
}

/** Shows the package value in use until the next restart. */
void restart_tooltip(const ConfigValue& applied) noexcept {
    switch (applied.type) {
    case ConfigType::integer:
        ImGui::SetItemTooltip("Applied at startup: %d", applied.integer);
        break;
    case ConfigType::floating:
        ImGui::SetItemTooltip("Applied at startup: %.9g", static_cast<double>(applied.floating));
        break;
    case ConfigType::boolean:
        ImGui::SetItemTooltip("Applied at startup: %s", applied.boolean ? "on" : "off");
        break;
    }
}

/**
 * Draws one config entry: its control, a reset button and its label.
 * @param modId Mod that declares the entry.
 * @param entry The declaration.
 * @param edit Receives a finished change.
 */
void draw_entry(const state::mods::Id& modId, const ConfigEntry& entry, Edit& edit) noexcept {
    ConfigValue value{};
    const bool saved = state::mods::config::current(modId.view(), entry, value);
    const ConfigValue shown = value;
    const bool finished = draw_value(value);
    ImGui::SameLine();
    ImGui::BeginDisabled(!saved);
    const bool reset = ImGui::SmallButton("Reset");
    ImGui::EndDisabled();
    ImGui::SameLine();
    const std::string_view label = entry.name.length != 0 ? entry.name.view() : entry.key.view();
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    default_tooltip(entry);
    ConfigValue applied{};
    if (state::mods::config::applied(modId.view(), entry.key.view(), applied)
        && state::mods::config::restart_required(modId.view(), entry)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart)");
        restart_tooltip(applied);
    }
    if (finished && !state::mods::config::same_value(value, shown)) {
        edit = {EditKind::write, modId, entry.key, value};
    } else if (reset) {
        edit = {EditKind::reset, modId, entry.key, entry.defaultValue};
    }
}

/**
 * Draws one control per config entry of a loaded mod.
 * @param manifest The mod.
 * @param edit Receives a finished change.
 */
void draw_config(const state::mods::Manifest& manifest, Edit& edit) noexcept {
    if (manifest.configCount == 0) {
        return;
    }
    ImGui::TextDisabled("Config");
    const std::string_view id = manifest.id.view();
    ImGui::PushID(id.data(), id.data() + id.size());
    for (std::size_t index = 0; index < manifest.configCount; ++index) {
        ImGui::PushID(static_cast<int>(index));
        draw_entry(manifest.id, manifest.config[index], edit);
        ImGui::PopID();
    }
    ImGui::PopID();
}

/**
 * Draws one mod as a collapsible entry. Refused mods start open, so the reason is visible.
 * @param record Mod to draw.
 * @param index Position in the list; it keeps each header's identity stable.
 * @param loaded True for a mod that loaded.
 * @param edit Receives a finished config change.
 */
void draw_record(const Record& record, std::size_t index, bool loaded, Edit& edit) noexcept {
    std::array<char, kLabelCapacity> label{};
    const std::string_view name =
        record.manifestRead ? record.manifest.name.view() : record.folder.view();
    const auto version = state::mods::version::text(record.manifest.version);
    (void)std::snprintf(label.data(),
                        label.size(),
                        "%.*s  %s%s%s##mod%zu",
                        static_cast<int>(name.size()),
                        name.data(),
                        record.manifestRead ? version.data() : "",
                        record.manifestRead && record.manifest.cheat ? "  (cheat)" : "",
                        loaded ? "" : "  (not loaded)",
                        index);
    ImGui::SetNextItemOpen(!loaded, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader(label.data())) {
        return;
    }
    ImGui::Indent();
    if (record.manifestRead) {
        field("Id", record.manifest.id.view());
        draw_authors(record.manifest);
        wrapped(record.manifest.description.view());
        field("Source", record.manifest.source.view());
        draw_relations(record.manifest);
    }
    field("Folder", record.folder.view());
    if (loaded) {
        draw_contents(record);
        draw_config(record.manifest, edit);
    }
    draw_problems(record);
    ImGui::Unindent();
}

/**
 * Draws the summary and every mod from the published list.
 * @param context The frame's `Edit`, which receives a finished config change.
 * @param view Published mod list.
 * @return Always true; the page drew the view.
 */
[[nodiscard]] bool draw_view(void* context, const state::mods::View& view) noexcept {
    auto& edit = *static_cast<Edit*>(context);
    ImGui::Text("%zu loaded, %zu not loaded", view.loadedCount, view.count - view.loadedCount);
    const auto loaded = std::span(view.records, view.loadedCount);
    const auto cheats = std::count_if(
        loaded.begin(), loaded.end(), [](const Record& record) { return record.manifest.cheat; });
    if (cheats != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        ImGui::TextWrapped("%zu cheat mods are loaded. Progress in this session is not saved.",
                           static_cast<std::size_t>(cheats));
        ImGui::PopStyleColor();
    }
    if (view.mergedPackages != 0) {
        ImGui::TextDisabled("%zu merged packages", view.mergedPackages);
        ImGui::SetItemTooltip("Packages patched by more than one mod are merged into one file.");
    }
    std::array<char, kFolderTextCapacity> folder{};
    const int folderBytes = WideCharToMultiByte(CP_UTF8,
                                                0,
                                                view.modsDirectory.data(),
                                                static_cast<int>(view.modsDirectory.size()),
                                                folder.data(),
                                                static_cast<int>(folder.size() - 1),
                                                nullptr,
                                                nullptr);
    if (folderBytes > 0) {
        field("Mods folder", {folder.data(), static_cast<std::size_t>(folderBytes)});
        ImGui::SetItemTooltip("One folder per mod. Restart after changes.");
    }
    ImGui::Spacing();
    if (view.count == 0) {
        ImGui::TextDisabled("No mods installed.");
        return true;
    }
    for (std::size_t index = 0; index < view.count; ++index) {
        draw_record(view.records[index], index, index < view.loadedCount, edit);
    }
    return true;
}

/** Applies the change made this frame, after the mod list lock is released. */
void apply(const Edit& edit) noexcept {
    // The store logs its own failures; the page shows the stored value next frame.
    if (edit.kind == EditKind::write) {
        (void)state::mods::config::write(edit.modId.view(), edit.key.view(), edit.value);
    } else if (edit.kind == EditKind::reset) {
        (void)state::mods::config::reset(edit.modId.view(), edit.key.view());
    }
}

} // namespace

/** Draws the Mods page inside the active Core UI frame. */
void draw() noexcept {
    components::section::header("Mods", "Package values apply after a restart.");
    ImGui::Spacing();
    Edit edit{};
    if (!state::mods::visit(&draw_view, &edit)) {
        ImGui::TextDisabled("The mod list is not ready.");
        return;
    }
    apply(edit);
}

} // namespace sunrise::core::ui::modules::mods::internal
