#include "spawn_inspection_provider.h"

#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/scenarios/scenario_catalog.h"
#include "../../../state/build_data/spawn_sets/spawn_set_catalog.h"

namespace sunrise::client::inspection::providers {
namespace {

namespace activity = state::activity;
namespace layouts = state::build_data::scenarios;
namespace spawn_sets = state::build_data::spawn_sets;
namespace live = live_player;
namespace placed = placed_objects;
namespace observed = runtime_observations;
namespace tables = middleware::content::packages::tables;

[[nodiscard]] std::string
package_name(const activity::destination::DestinationSelection& destination) {
    const auto* bytes = reinterpret_cast<const char*>(destination.packageName.data());
    return std::string(bytes, destination.packageNameLength);
}

[[nodiscard]] std::string stem_name(const layouts::Definition& layout) {
    return std::string(layout.spawnStem.data(), layout.spawnStemLength);
}

[[nodiscard]] std::string hex_label(const char* prefix, std::uint32_t value) {
    std::array<char, 64> text{};
    const int written = std::snprintf(text.data(), text.size(), "%s 0x%08X", prefix, value);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string(prefix);
}

[[nodiscard]] std::string session_label(std::uint64_t value) {
    std::array<char, 64> text{};
    const int written =
        std::snprintf(text.data(),
                      text.size(),
                      "Activity session 0x%016llX",
                      static_cast<unsigned long long>(value));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Activity session");
}

[[nodiscard]] std::string destination_label(std::int32_t activityIndex) {
    if (activityIndex < 0) {
        return "Destination";
    }
    std::array<char, 48> text{};
    const int written =
        std::snprintf(text.data(), text.size(), "Destination %d", activityIndex);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Destination");
}

[[nodiscard]] std::string spawn_label(std::size_t ordinal) {
    std::array<char, 48> text{};
    const int written = std::snprintf(text.data(), text.size(), "Spawn point %04zu", ordinal + 1);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Spawn point");
}

void add_capability_diagnostics(std::vector<Diagnostic>& diagnostics) {
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Inspector provider coverage is activity, destination, scenario, package-backed roster "
         "placements, and spawn-catalog data."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Roster-backed placed-object inspection is partial entity coverage. It does not prove "
         "that a live object-system handle or simulation entity currently exists."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Spawn inspection is supported; availability depends on the current scenario and spawn "
         "catalogs."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Live runtime coverage proves the local controlled-object handle and its published physics "
         "position when the player observer is ready."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "The primary Wwise listener and bounded Havok body slots are copied from their existing "
         "runtime hooks. Physics slots are observations, not durable object identities."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "The engine's occupied object-datum iterator now provides general live object identities "
         "and object-system types. StaticMesh and Speedtree objects populate Geometry."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Terrain surfaces, trigger volume bounds, audio emitters, lights, physics controllers, and "
         "stable physics-body identities still need separate native producers."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Object transforms, bounds, and depth-assisted surface picking are unavailable for "
         "package-backed placement nodes."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Hide and isolate affect inspector helpers, not game rendering."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Raw source offsets and serialized reference edges are not retained by the current "
         "catalogs."});
}

void add_world_context_diagnostics(WorldSnapshot& snapshot) {
    const int bubble = snapshot.bubble ? static_cast<int>(*snapshot.bubble) : -1;
    const int mapBubble = snapshot.mapBubble ? static_cast<int>(*snapshot.mapBubble) : -1;
    const char* state = snapshot.stale ? "stale/deferred"
                                       : (snapshot.sessionPresent ? "current" : "unavailable");

    std::array<char, 704> text{};
    const int written = std::snprintf(
        text.data(),
        text.size(),
        "World context: package='%s' map='%s' session=0x%016llX revision=%llu activity=%d "
        "region=%d bubble=%d mapBubble=%d scenario=0x%08X spawnSet=0x%08X "
        "localPlayer=0x%08X localPlayerPresent=%s placedObjects=%zu runtimeObjects=%u/%u "
        "audioListener=%s "
        "physicsBodies=%u/%u state=%s.",
        snapshot.packageName.c_str(),
        snapshot.mapStem.c_str(),
        static_cast<unsigned long long>(snapshot.activitySession),
        static_cast<unsigned long long>(snapshot.activityRevision),
        snapshot.activityIndex,
        snapshot.region,
        bubble,
        mapBubble,
        snapshot.scenarioTag,
        snapshot.spawnSetHash,
        snapshot.localPlayerHandle,
        snapshot.localPlayerPresent ? "yes" : "no",
        snapshot.placedObjectCount,
        static_cast<unsigned>(snapshot.runtimeObjectCount),
        snapshot.runtimeObjectDeclaredCount,
        snapshot.audioListenerPresent ? "yes" : "no",
        static_cast<unsigned>(snapshot.physicsBodyCount),
        snapshot.physicsDeclaredSlots,
        state);
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        snapshot.diagnostics.push_back(
            {Diagnostic::Severity::information,
             std::string(text.data(), static_cast<std::size_t>(written))});
    }

    snapshot.diagnostics.push_back(
        {Diagnostic::Severity::information,
         std::string("Catalog readiness: scenario=")
             + (snapshot.scenarioCatalogReady ? "ready" : "not ready") + ", spawns="
             + (snapshot.spawnCatalogReady ? "ready" : "not ready") + "."});
}

} // namespace
