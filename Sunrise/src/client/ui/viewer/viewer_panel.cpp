#include "viewer_panel.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <system_error>

#include "../../../core/settings/settings.h"
#include "../../../core/ui/components/label/ui_label_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../../core/ui/modules/logs/logs.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scenarios/scenario_catalog.h"
#include "../../../state/build_data/spawn_sets/spawn_set_catalog.h"
#include "../../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../hooks/player_hold/player_hold.h"
#include "../../hooks/viewer_audio/viewer_audio.h"
#include "../../hooks/viewer_camera/viewer_camera.h"
#include "../../player/player_position.h"
#include "../../viewer/viewer_camera_settings_store.h"
#include "../../viewer/viewer_camera_path_store.h"
#include "../components/key_picker/client_key_picker.h"
#include "../world_inspector/world_inspector.h"

namespace sunrise::client::ui::viewer {
namespace {

namespace activity = state::activity;
namespace camera = client::viewer::camera;
namespace clipboard = core::ui::modules::logs;
namespace key_picker = client::ui::components::key_picker;
namespace camera_paths = client::viewer::paths;
namespace label = core::ui::components::label;
namespace layouts = state::build_data::scenarios;
namespace spawn_sets = state::build_data::spawn_sets;
namespace tables = middleware::content::packages::tables;

constexpr std::size_t kNoBubble = static_cast<std::size_t>(-1);
constexpr std::size_t kDiagnosticCapacity = 4096;

struct WorldContext {
    activity::SessionSnapshot activity{};
    activity::destination::DestinationSelection destination{};
    layouts::Definition layout{};
    spawn_sets::NameHash spawnSet{};
    client::player::position::Snapshot player{};
    camera::Status camera{};
    std::uint64_t cameraSession{};
    std::uint64_t activityBindingGeneration{};
    std::int32_t region{-1};
    std::size_t bubble{kNoBubble};
    std::uint32_t effectiveSpawnSetHash{};
    std::uint8_t sliceState{};
    bool sessionPresent{};
    bool sessionFromPrivateBinding{};
    bool sessionFromRegion{};
    bool destinationPresent{};
    bool scenarioCatalogReady{};
    bool scenarioPresent{};
    bool scenarioCapacityLimited{};
    bool regionPresent{};
    bool bubblePresent{};
    bool spawnSetHashPresent{};
    bool spawnSetCatalogReady{};
    bool spawnSetPresent{};
    bool spawnSetCapacityLimited{};
    bool inWorld{};
    bool privateOverride{};
    bool playerHold{};
    bool audioInstalled{};
    bool audioApplied{};
    bool stale{};
};

[[nodiscard]] std::string_view
package_name(const activity::destination::DestinationSelection& destination) noexcept {
    return {reinterpret_cast<const char*>(destination.packageName.data()),
            destination.packageNameLength};
}

using EscapedPackage = std::array<char, activity::destination::kPackageNameCapacity * 6 + 1>;

[[nodiscard]] std::string_view
json_package_name(const activity::destination::DestinationSelection& destination,
                  EscapedPackage& output) noexcept {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t used = 0;
    for (const unsigned char value : package_name(destination)) {
        if (value == '"' || value == '\\') {
            output[used++] = '\\';
            output[used++] = static_cast<char>(value);
        } else if (value >= 0x20 && value <= 0x7E) {
            output[used++] = static_cast<char>(value);
        } else {
            output[used++] = '\\';
            output[used++] = 'u';
            output[used++] = '0';
            output[used++] = '0';
            output[used++] = kHex[value >> 4];
            output[used++] = kHex[value & 0x0F];
        }
    }
    output[used] = '\0';
    return {output.data(), used};
}

[[nodiscard]] WorldContext read_world_context(const camera::Status& cameraStatus) noexcept {
    WorldContext context{};
    context.camera = cameraStatus;
    context.cameraSession = cameraStatus.activeSession;
    context.player = client::player::position::snapshot();
    context.inWorld = hooks::bootflow::in_world();
    context.privateOverride = core::settings::get().client.regionPrivate;
    context.playerHold = hooks::player_hold::holding();
    context.audioInstalled = client::viewer::audio::installed();
    context.audioApplied = client::viewer::audio::applied();

    server::bap::ActivitySnapshot privateActivity{};
    context.sessionFromPrivateBinding = server::bap::snapshot_private_activity(privateActivity);
    if (context.sessionFromPrivateBinding) {
        context.activityBindingGeneration = privateActivity.bindingGeneration;
        context.sessionPresent =
            activity::snapshot_session(privateActivity.binding.sessionId, context.activity);
        context.stale =
            !context.sessionPresent
            || context.activity.binding.sessionId != privateActivity.binding.sessionId
            || context.activity.binding.createdRevision != privateActivity.binding.createdRevision;
    } else {
        const std::uint64_t regionSession =
            activity::membership::live_region_session(activity::kAbsentSessionId);
        context.sessionFromRegion = regionSession != activity::kAbsentSessionId;
        context.sessionPresent = context.sessionFromRegion
                                 && activity::snapshot_session(regionSession, context.activity);
        context.stale = context.sessionFromRegion && !context.sessionPresent;
    }
    if (!context.sessionPresent) {
        return context;
    }

    context.destination = context.activity.binding.destination;
    context.destinationPresent = true;
    context.region = context.activity.reportedRegion;
    context.regionPresent = context.region >= 0;
    context.scenarioCatalogReady = state::build_data::scenario_layouts_ready();
    if (context.destinationPresent && context.scenarioCatalogReady) {
        const std::string_view name = package_name(context.destination);
        context.scenarioPresent = layouts::find(name, context.layout);
        context.scenarioCapacityLimited = context.scenarioPresent && context.layout.truncated != 0;
    }

    if (context.scenarioPresent && context.regionPresent) {
        context.bubble = static_cast<std::size_t>(context.region) / tables::kSliceSetIndexFactor;
        context.sliceState = static_cast<std::uint8_t>(static_cast<std::uint32_t>(context.region)
                                                       % tables::kSliceSetIndexFactor);
        context.bubblePresent = context.bubble < context.layout.bubbleCount;
    }

    if (context.destinationPresent) {
        if (context.destination.hasSpawnSetOverride) {
            context.effectiveSpawnSetHash = context.destination.spawnSetOverride;
            context.spawnSetHashPresent =
                activity::destination::usable_spawn_set_hash(true, context.effectiveSpawnSetHash);
        } else if (activity::destination::usable_spawn_set_hash(context.destination.hasSpawnSetHash,
                                                                context.destination.spawnSetHash)) {
            context.effectiveSpawnSetHash = context.destination.spawnSetHash;
            context.spawnSetHashPresent = true;
        }
    }
    context.spawnSetCatalogReady = state::build_data::spawn_sets_ready();
    if (context.spawnSetHashPresent && context.scenarioPresent && context.spawnSetCatalogReady) {
        const std::string_view stem{context.layout.spawnStem.data(),
                                    context.layout.spawnStemLength};
        context.spawnSetPresent =
            spawn_sets::find_hash(stem, context.effectiveSpawnSetHash, context.spawnSet);
        context.spawnSetCapacityLimited =
            context.spawnSetPresent && context.spawnSet.activityPackageOverflow != 0;
    }

    bool sourceCurrent = false;
    if (context.sessionFromPrivateBinding) {
        server::bap::ActivitySnapshot current{};
        sourceCurrent =
            server::bap::snapshot_private_activity(current)
            && current.bindingGeneration == context.activityBindingGeneration
            && current.binding.sessionId == context.activity.binding.sessionId
            && current.binding.createdRevision == context.activity.binding.createdRevision;
    } else if (context.sessionFromRegion) {
        sourceCurrent = activity::membership::live_region_session(activity::kAbsentSessionId)
                        == context.activity.binding.sessionId;
    }
    context.stale =
        context.stale || !sourceCurrent || !activity::binding_matches(context.activity.binding);
    return context;
}

[[nodiscard]] const char* snapshot_state(const WorldContext& context) noexcept {
    if (context.stale) {
        return "stale";
    }
    if (!context.sessionPresent) {
        return "unavailable";
    }
    if (!context.activity.joined || !context.destinationPresent || !context.scenarioCatalogReady
        || (context.spawnSetHashPresent && !context.spawnSetCatalogReady)
        || (context.inWorld && !context.regionPresent)) {
        return "incomplete";
    }
    if (context.scenarioCapacityLimited || context.spawnSetCapacityLimited) {
        return "capacity-limited";
    }
    return "ready";
}

[[nodiscard]] const char* activity_source(const WorldContext& context) noexcept {
    if (context.sessionFromPrivateBinding) {
        return "private-binding";
    }
    return context.sessionFromRegion ? "reported-region" : "unavailable";
}

[[nodiscard]] const char* boolean(bool value) noexcept {
    return value ? "true" : "false";
}

using JsonScalar = std::array<char, 32>;

void json_scalar(float value, JsonScalar& output) noexcept {
    if (std::isfinite(value)) {
        const std::to_chars_result converted =
            std::to_chars(output.data(),
                          output.data() + output.size() - 1,
                          value,
                          std::chars_format::general,
                          std::numeric_limits<float>::max_digits10);
        if (converted.ec == std::errc{}) {
            *converted.ptr = '\0';
            return;
        }
    }
    output = {};
    output[0] = 'n';
    output[1] = 'u';
    output[2] = 'l';
    output[3] = 'l';
}

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
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        (void)clipboard::queue_text_copy(
            std::string_view(text.data(), static_cast<std::size_t>(written)));
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

[[nodiscard]] bool compact_diagnostics(const WorldContext& context,
                                       std::array<char, kDiagnosticCapacity>& output) noexcept {
    const std::string_view package =
        context.destinationPresent ? package_name(context.destination) : std::string_view{};
    const long long bubble = context.bubblePresent ? static_cast<long long>(context.bubble) : -1;
    const int written = std::snprintf(
        output.data(),
        output.size(),
        "state=%s session=0x%016llX source=%s joined=%u generation=%llu "
        "record_revision=%llu package=%.*s activity=%d element=%d reason=%d "
        "arrival_hash=0x%08X spawn_hash=0x%08X in_world=%u region=%d bubble=%lld "
        "map_bubble=%u slice_set=%d slice_state=%u scenario_state=0x%02X "
        "player_present=%u player=(%.3f,%.3f,%.3f) viewer_session=%llu "
        "camera_active=%u camera=(%.3f,%.3f,%.3f) fov=%.2f hold=%u audio=%u",
        snapshot_state(context),
        static_cast<unsigned long long>(context.activity.binding.sessionId),
        activity_source(context),
        context.activity.joined ? 1U : 0U,
        static_cast<unsigned long long>(context.activity.binding.createdRevision),
        static_cast<unsigned long long>(context.activity.recordRevision),
        static_cast<int>(package.size()),
        package.data(),
        static_cast<int>(context.destination.activityIndex),
        context.destination.hasElementIndex ? static_cast<int>(context.destination.elementIndex)
                                            : -1,
        static_cast<int>(context.destination.reason),
        context.destination.hasArrivalBubbleHash ? context.destination.arrivalBubbleHash : 0U,
        context.spawnSetHashPresent ? context.effectiveSpawnSetHash : 0U,
        context.inWorld ? 1U : 0U,
        context.regionPresent ? context.region : -1,
        bubble,
        context.bubblePresent ? context.layout.bubbleMapIndices[context.bubble] : 0U,
        context.regionPresent ? context.region : -1,
        static_cast<unsigned>(context.sliceState),
        context.bubblePresent ? context.layout.bubbleStates[context.bubble] : 0U,
        context.player.present ? 1U : 0U,
        static_cast<double>(context.player.position[0]),
        static_cast<double>(context.player.position[1]),
        static_cast<double>(context.player.position[2]),
        static_cast<unsigned long long>(context.cameraSession),
        context.camera.active ? 1U : 0U,
        static_cast<double>(context.camera.pose.position[0]),
        static_cast<double>(context.camera.pose.position[1]),
        static_cast<double>(context.camera.pose.position[2]),
        static_cast<double>(context.camera.pose.fov),
        context.playerHold ? 1U : 0U,
        context.audioApplied ? 1U : 0U);
    return written > 0 && static_cast<std::size_t>(written) < output.size();
}

[[nodiscard]] bool json_diagnostics(const WorldContext& context,
                                    std::array<char, kDiagnosticCapacity>& output) noexcept {
    EscapedPackage packageStorage{};
    const std::string_view package = context.destinationPresent
                                         ? json_package_name(context.destination, packageStorage)
                                         : std::string_view{};
    const std::array<float, 10> scalarValues{
        context.player.position[0],
        context.player.position[1],
        context.player.position[2],
        context.camera.pose.position[0],
        context.camera.pose.position[1],
        context.camera.pose.position[2],
        context.camera.pose.forward[0],
        context.camera.pose.forward[1],
        context.camera.pose.forward[2],
        context.camera.pose.fov,
    };
    std::array<JsonScalar, 10> scalars{};
    for (std::size_t index = 0; index < scalars.size(); ++index) {
        json_scalar(scalarValues[index], scalars[index]);
    }
    const int written = std::snprintf(
        output.data(),
        output.size(),
        "{\n"
        "  \"snapshot_state\": \"%s\",\n"
        "  \"activity\": {\"present\": %s, \"source\": \"%s\", "
        "\"session_id\": \"0x%016llX\", \"joined\": %s, \"state_revision\": %llu, "
        "\"record_revision\": %llu, \"joined_revision\": %llu, \"generation\": %llu},\n"
        "  \"destination\": {\"present\": %s, \"package\": \"%.*s\", "
        "\"activity_index\": %d, \"element_present\": %s, \"element_index\": %d, "
        "\"reason\": %d, \"arrival_hash_present\": %s, \"arrival_hash\": \"0x%08X\", "
        "\"arrival_override_present\": %s, \"arrival_override\": %u, "
        "\"spawn_hash_present\": %s, \"spawn_hash\": \"0x%08X\", "
        "\"spawn_override_present\": %s, \"spawn_override\": \"0x%08X\", "
        "\"effective_spawn_present\": %s, \"effective_spawn\": \"0x%08X\"},\n"
        "  \"world\": {\"in_world\": %s, \"region_present\": %s, \"region\": %d, "
        "\"privacy_runtime\": \"unavailable\", \"private_override\": %s, "
        "\"scenario_catalog_ready\": %s, \"scenario_present\": %s, "
        "\"capacity_limited\": %s, \"bubble_present\": %s, \"bubble\": %lld, "
        "\"map_bubble\": %u, \"bubble_hash\": \"0x%08X\", \"bubble_state\": %u, "
        "\"slice_set\": %d, \"slice_state\": %u, \"slice_state_count\": %u},\n"
        "  \"spawn_catalog\": {\"ready\": %s, \"set_present\": %s, "
        "\"capacity_limited\": %s, \"point_count\": %u},\n"
        "  \"player\": {\"present\": %s, \"position\": [%s, %s, %s]},\n"
        "  \"viewer\": {\"installed\": %s, \"requested\": %s, \"active\": %s, "
        "\"applied\": %s, \"session\": %llu, \"generation\": %llu, "
        "\"position\": [%s, %s, %s], \"forward\": [%s, %s, %s], "
        "\"fov\": %s, \"player_hold\": %s, \"audio_installed\": %s, "
        "\"audio_applied\": %s}\n"
        "}\n",
        snapshot_state(context),
        boolean(context.sessionPresent),
        activity_source(context),
        static_cast<unsigned long long>(context.activity.binding.sessionId),
        boolean(context.activity.joined),
        static_cast<unsigned long long>(context.activity.stateRevision),
        static_cast<unsigned long long>(context.activity.recordRevision),
        static_cast<unsigned long long>(context.activity.joinedRevision),
        static_cast<unsigned long long>(context.activity.binding.createdRevision),
        boolean(context.destinationPresent),
        static_cast<int>(package.size()),
        package.data(),
        static_cast<int>(context.destination.activityIndex),
        boolean(context.destination.hasElementIndex),
        static_cast<int>(context.destination.elementIndex),
        static_cast<int>(context.destination.reason),
        boolean(context.destination.hasArrivalBubbleHash),
        context.destination.arrivalBubbleHash,
        boolean(context.destination.hasArrivalBubbleOverride),
        static_cast<unsigned>(context.destination.arrivalBubbleOverride),
        boolean(context.destination.hasSpawnSetHash),
        context.destination.spawnSetHash,
        boolean(context.destination.hasSpawnSetOverride),
        context.destination.spawnSetOverride,
        boolean(context.spawnSetHashPresent),
        context.effectiveSpawnSetHash,
        boolean(context.inWorld),
        boolean(context.regionPresent),
        context.regionPresent ? context.region : -1,
        boolean(context.privateOverride),
        boolean(context.scenarioCatalogReady),
        boolean(context.scenarioPresent),
        boolean(context.scenarioCapacityLimited),
        boolean(context.bubblePresent),
        context.bubblePresent ? static_cast<long long>(context.bubble) : -1,
        context.bubblePresent ? context.layout.bubbleMapIndices[context.bubble] : 0U,
        context.bubblePresent ? context.layout.bubbleHashes[context.bubble] : 0U,
        context.bubblePresent ? static_cast<unsigned>(context.layout.bubbleStates[context.bubble])
                              : 0U,
        context.regionPresent ? context.region : -1,
        static_cast<unsigned>(context.sliceState),
        context.bubblePresent
            ? static_cast<unsigned>(context.layout.bubbleStateCounts[context.bubble])
            : 0U,
        boolean(context.spawnSetCatalogReady),
        boolean(context.spawnSetPresent),
        boolean(context.spawnSetCapacityLimited),
        context.spawnSetPresent ? context.spawnSet.pointCount : 0U,
        boolean(context.player.present),
        scalars[0].data(),
        scalars[1].data(),
        scalars[2].data(),
        boolean(context.camera.installed),
        boolean(context.camera.requested),
        boolean(context.camera.active),
        boolean(context.camera.applied),
        static_cast<unsigned long long>(context.cameraSession),
        static_cast<unsigned long long>(context.camera.generation),
        scalars[3].data(),
        scalars[4].data(),
        scalars[5].data(),
        scalars[6].data(),
        scalars[7].data(),
        scalars[8].data(),
        scalars[9].data(),
        boolean(context.playerHold),
        boolean(context.audioInstalled),
        boolean(context.audioApplied));
    return written > 0 && static_cast<std::size_t>(written) < output.size();
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

[[nodiscard]] camera::PlaybackPath runtime_path(const camera_paths::CameraPath& path,
                                                const WorldContext& world) noexcept {
    camera::PlaybackPath result{};
    result.keyframeCount = (std::min)(path.keyframes.size(), result.keyframes.size());
    result.loop = path.loop;
    std::snprintf(result.name.data(), result.name.size(), "%s", path.name.c_str());
    if (world.sessionPresent) {
        result.activitySession = world.activity.binding.sessionId;
        result.activityRevision = world.activity.binding.createdRevision;
    }
    for (std::size_t index = 0; index < result.keyframeCount; ++index) {
        const camera_paths::Keyframe& source = path.keyframes[index];
        camera::PlaybackKeyframe& destination = result.keyframes[index];
        destination.pose.position = source.position;
        destination.pose.yaw = source.yaw;
        destination.pose.pitch = source.pitch;
        destination.pose.fov = source.fov;
        std::snprintf(destination.label.data(), destination.label.size(), "%s",
                      source.label.c_str());
        destination.travelSeconds = source.travelSeconds;
        destination.dwellSeconds = source.dwellSeconds;
        destination.captureSnapshot = source.captureSnapshot;
    }
    return result;
}

void draw_camera_paths(const camera::Status& status, const WorldContext& world) noexcept {
    static std::size_t selectedPath = 0;
    camera_paths::Library library = camera_paths::get();
    if (selectedPath >= library.paths.size()) {
        selectedPath = library.paths.empty() ? 0 : library.paths.size() - 1;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Camera Paths");
    ImGui::Separator();
    if (ImGui::Button("New path") && library.paths.size() < camera_paths::kMaximumPathCount) {
        camera_paths::CameraPath path{};
        path.name = "Path " + std::to_string(library.paths.size() + 1);
        library.paths.push_back(std::move(path));
        if (camera_paths::publish(library)) {
            selectedPath = library.paths.size() - 1;
        }
    }
    if (library.paths.empty()) {
        ImGui::TextDisabled("Create a path, then record the current Viewer pose.");
        return;
    }

    ImGui::SameLine();
    std::array<const char*, camera_paths::kMaximumPathCount> names{};
    for (std::size_t index = 0; index < library.paths.size(); ++index) {
        names[index] = library.paths[index].name.c_str();
    }
    int selected = static_cast<int>(selectedPath);
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::Combo("##camera_path", &selected, names.data(),
                     static_cast<int>(library.paths.size()))) {
        selectedPath = static_cast<std::size_t>(selected);
    }

    camera_paths::CameraPath& path = library.paths[selectedPath];
    bool persist = false;
    std::array<char, camera_paths::kMaximumPathNameBytes + 1> name{};
    std::snprintf(name.data(), name.size(), "%s", path.name.c_str());
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::InputText("Name##camera_path_name", name.data(), name.size())) {
        path.name = name.data();
        persist = true;
    }
    ImGui::SameLine();
    persist = ImGui::Checkbox("Loop##camera_path_loop", &path.loop) || persist;
    ImGui::SameLine();
    if (ImGui::Button("Delete path")) {
        library.paths.erase(library.paths.begin() + static_cast<std::ptrdiff_t>(selectedPath));
        selectedPath = 0;
        (void)camera_paths::publish(library);
        camera::request_playback_stop();
        return;
    }

    ImGui::BeginDisabled(!status.active
                         || path.keyframes.size() >= camera_paths::kMaximumKeyframeCount);
    if (ImGui::Button("Record current pose")) {
        camera_paths::Keyframe keyframe{};
        keyframe.position = status.pose.position;
        keyframe.yaw = status.pose.yaw;
        keyframe.pitch = status.pose.pitch;
        keyframe.fov = std::clamp(status.pose.fov, camera_paths::kMinimumKeyframeFov,
                                  camera_paths::kMaximumKeyframeFov);
        keyframe.label = "Keyframe " + std::to_string(path.keyframes.size() + 1);
        world_inspector::SelectionIdentity selection{};
        if (world_inspector::selected_identity(selection)) {
            keyframe.selection = camera_paths::SelectionIdentity{selection.producerEpoch,
                                                                  selection.nativeKey,
                                                                  selection.producer,
                                                                  selection.kind};
        }
        path.keyframes.push_back(std::move(keyframe));
        persist = true;
    }
    ImGui::EndDisabled();

    const camera::PlaybackStatus playback = camera::playback_status();
    ImGui::SameLine();
    ImGui::BeginDisabled(!status.active || path.keyframes.empty());
    if (ImGui::Button("Play")) {
        (void)camera::request_playback(runtime_path(path, world));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!playback.playing);
    if (ImGui::Button(playback.paused ? "Resume" : "Pause")) {
        camera::request_playback_pause(!playback.paused);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        camera::request_playback_stop();
    }
    ImGui::EndDisabled();
    if (playback.playing && playback.durationSeconds > 0.0F) {
        float scrub = playback.elapsedSeconds;
        ImGui::SetNextItemWidth((std::max)(160.0F, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderFloat("Scrub##camera_path", &scrub, 0.0F,
                               playback.durationSeconds, "%.2fs")) {
            camera::request_playback_scrub(scrub);
        }
    }

    for (std::size_t index = 0; index < path.keyframes.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        camera_paths::Keyframe& keyframe = path.keyframes[index];
        std::array<char, camera_paths::kMaximumKeyframeLabelBytes + 1> labelText{};
        std::snprintf(labelText.data(), labelText.size(), "%s", keyframe.label.c_str());
        ImGui::Separator();
        ImGui::Text("%zu", index + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0F);
        if (ImGui::InputText("##keyframe_label", labelText.data(), labelText.size())) {
            keyframe.label = labelText.data();
            persist = true;
        }
        ImGui::SameLine();
        persist = ImGui::DragFloat("Travel", &keyframe.travelSeconds, 0.05F, 0.0F,
                                   camera_paths::kMaximumSegmentSeconds, "%.2fs") || persist;
        ImGui::SameLine();
        persist = ImGui::DragFloat("Dwell", &keyframe.dwellSeconds, 0.05F, 0.0F,
                                   camera_paths::kMaximumSegmentSeconds, "%.2fs") || persist;
        persist = ImGui::Checkbox("Capture snapshot", &keyframe.captureSnapshot) || persist;
        ImGui::SameLine();
        if (ImGui::SmallButton("Up") && index > 0) {
            std::swap(path.keyframes[index], path.keyframes[index - 1]);
            persist = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Down") && index + 1 < path.keyframes.size()) {
            std::swap(path.keyframes[index], path.keyframes[index + 1]);
            persist = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Duplicate")
            && path.keyframes.size() < camera_paths::kMaximumKeyframeCount) {
            const camera_paths::Keyframe copy = keyframe;
            path.keyframes.insert(path.keyframes.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                  copy);
            persist = true;
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            path.keyframes.erase(path.keyframes.begin() + static_cast<std::ptrdiff_t>(index));
            persist = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (persist) {
        (void)camera_paths::publish(library);
    }
}

void draw_world_context(const WorldContext& context) noexcept {
    ImGui::Spacing();
    ImGui::TextUnformatted("World Context");
    ImGui::Separator();
    ImGui::Text("Snapshot: %s", snapshot_state(context));
    if (context.stale) {
        ImGui::TextWrapped(
            "Stale: the activity generation or destination binding changed while data was copied.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Activity and destination");
    ImGui::Separator();
    if (!context.sessionPresent) {
        ImGui::TextUnformatted("Unavailable: no committed activity session.");
    } else {
        ImGui::Text("Session: 0x%016llX (%s)",
                    static_cast<unsigned long long>(context.activity.binding.sessionId),
                    activity_source(context));
        ImGui::Text("Join: %s", context.activity.joined ? "joined" : "incomplete");
        ImGui::Text("Generation: %llu",
                    static_cast<unsigned long long>(context.activity.binding.createdRevision));
        ImGui::Text("Revisions: state %llu | record %llu | join %llu",
                    static_cast<unsigned long long>(context.activity.stateRevision),
                    static_cast<unsigned long long>(context.activity.recordRevision),
                    static_cast<unsigned long long>(context.activity.joinedRevision));
    }
    if (!context.destinationPresent) {
        ImGui::TextUnformatted("Destination: unavailable.");
    } else {
        const std::string_view package = package_name(context.destination);
        ImGui::Text("Package: %.*s", static_cast<int>(package.size()), package.data());
        ImGui::Text("Activity: %d | Previous: %d | Reason: %d",
                    static_cast<int>(context.destination.activityIndex),
                    static_cast<int>(context.destination.previousActivityIndex),
                    static_cast<int>(context.destination.reason));
        if (context.destination.hasElementIndex) {
            ImGui::Text("Element: %d", static_cast<int>(context.destination.elementIndex));
        } else {
            ImGui::TextUnformatted("Element: unavailable");
        }
        if (context.destination.hasArrivalBubbleHash) {
            ImGui::Text("Arrival bubble hash: 0x%08X", context.destination.arrivalBubbleHash);
        } else {
            ImGui::TextUnformatted("Arrival bubble hash: unavailable");
        }
        if (context.destination.hasArrivalBubbleOverride) {
            ImGui::Text("Arrival bubble override: %u",
                        static_cast<unsigned>(context.destination.arrivalBubbleOverride));
        }
        if (context.destination.hasSpawnSetHash) {
            ImGui::Text("Spawn-set hash: 0x%08X", context.destination.spawnSetHash);
        } else {
            ImGui::TextUnformatted("Spawn-set hash: unavailable");
        }
        if (context.destination.hasSpawnSetOverride) {
            ImGui::Text("Spawn-set override: 0x%08X", context.destination.spawnSetOverride);
        }
        if (context.spawnSetHashPresent) {
            ImGui::Text("Effective spawn set: 0x%08X", context.effectiveSpawnSetHash);
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Scenario and region");
    ImGui::Separator();
    if (!context.scenarioCatalogReady) {
        ImGui::TextUnformatted("Scenario: incomplete - catalog is not ready.");
    } else if (!context.scenarioPresent) {
        ImGui::TextUnformatted("Scenario: unavailable for this package.");
    } else {
        ImGui::Text("Scenario: ready | bubbles %u | packages %u",
                    static_cast<unsigned>(context.layout.bubbleCount),
                    static_cast<unsigned>(context.layout.packageCount));
        if (context.scenarioCapacityLimited) {
            ImGui::TextUnformatted("Scenario: capacity-limited - bubble rows were truncated.");
        }
    }
    ImGui::Text("World: %s", context.inWorld ? "loaded" : "unavailable");
    if (context.regionPresent) {
        ImGui::Text("Region / slice set: %d", context.region);
    } else {
        ImGui::TextUnformatted("Region / slice set: incomplete - waiting for a report.");
    }
    ImGui::Text("Private-region override: %s", context.privateOverride ? "enabled" : "disabled");
    ImGui::TextUnformatted("Runtime public/private classification: unavailable");
    if (context.bubblePresent) {
        const std::size_t bubble = context.bubble;
        const char* bubbleState =
            context.layout.bubbleStates[bubble] == layouts::kBubbleEnabledByte
                ? "enabled"
                : (context.layout.bubbleStates[bubble] == layouts::kBubbleDisabledByte ? "disabled"
                                                                                       : "unknown");
        ImGui::Text("Current bubble: %zu | map-global: %u | hash: 0x%08X",
                    bubble,
                    context.layout.bubbleMapIndices[bubble],
                    context.layout.bubbleHashes[bubble]);
        ImGui::Text("Slice state: %u of %u | scenario state: %s (0x%02X)",
                    static_cast<unsigned>(context.sliceState),
                    static_cast<unsigned>(context.layout.bubbleStateCounts[bubble]),
                    bubbleState,
                    static_cast<unsigned>(context.layout.bubbleStates[bubble]));
    } else if (context.regionPresent) {
        ImGui::TextUnformatted(
            "Current bubble: unavailable - region is outside the copied layout.");
    } else {
        ImGui::TextUnformatted("Current bubble: incomplete - no reported region.");
    }
    if (!context.spawnSetHashPresent) {
        ImGui::TextUnformatted("Spawn-set catalog row: unavailable - no usable hash.");
    } else if (!context.spawnSetCatalogReady) {
        ImGui::TextUnformatted("Spawn-set catalog row: incomplete - catalog is not ready.");
    } else if (!context.spawnSetPresent) {
        ImGui::TextUnformatted("Spawn-set catalog row: unavailable for this map stem.");
    } else {
        ImGui::Text("Spawn-set catalog row: ready | points %u | activity packages %u",
                    context.spawnSet.pointCount,
                    static_cast<unsigned>(context.spawnSet.activityPackageCount));
        if (context.spawnSetCapacityLimited) {
            ImGui::TextUnformatted(
                "Spawn-set catalog row: capacity-limited - activity packages overflowed.");
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Runtime state");
    ImGui::Separator();
    if (context.player.present) {
        ImGui::Text("Player position: %.3f  %.3f  %.3f",
                    static_cast<double>(context.player.position[0]),
                    static_cast<double>(context.player.position[1]),
                    static_cast<double>(context.player.position[2]));
    } else {
        ImGui::TextUnformatted("Player position: unavailable");
    }
    ImGui::Text("Viewer session: %llu | generation: %llu",
                static_cast<unsigned long long>(context.cameraSession),
                static_cast<unsigned long long>(context.camera.generation));
    ImGui::Text("Viewer pose: %.3f  %.3f  %.3f | FOV %.2f",
                static_cast<double>(context.camera.pose.position[0]),
                static_cast<double>(context.camera.pose.position[1]),
                static_cast<double>(context.camera.pose.position[2]),
                static_cast<double>(context.camera.pose.fov));
    ImGui::Text("Player hold: %s", context.playerHold ? "holding" : "inactive / waiting");
    ImGui::Text("Audio listener: %s",
                context.audioApplied
                    ? "following camera"
                    : (context.audioInstalled ? "installed / waiting" : "unavailable"));

    ImGui::Spacing();
    ImGui::TextUnformatted("Diagnostics");
    ImGui::Separator();
    if (ImGui::Button("Copy compact diagnostics")) {
        std::array<char, kDiagnosticCapacity> text{};
        if (compact_diagnostics(context, text)) {
            (void)clipboard::queue_text_copy(text.data());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy JSON")) {
        std::array<char, kDiagnosticCapacity> text{};
        if (json_diagnostics(context, text)) {
            (void)clipboard::queue_text_copy(text.data());
        }
    }
}

} // namespace

void draw() noexcept {
    const camera::Status status = camera::status();
    const WorldContext world = read_world_context(status);
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
    if (ImGui::Button("Open World Inspector")) {
        world_inspector::open();
    }

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
    draw_camera_paths(status, world);
    draw_world_context(world);

    if (changed && !client::viewer::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::viewer
