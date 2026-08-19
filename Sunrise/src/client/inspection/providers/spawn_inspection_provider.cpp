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

[[nodiscard]] std::string spawn_label(std::size_t ordinal) {
    std::array<char, 48> text{};
    const int written = std::snprintf(text.data(), text.size(), "Spawn point %04zu", ordinal + 1);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Spawn point");
}

void add_capability_diagnostics(std::vector<Diagnostic>& diagnostics) {
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Runtime entity enumeration is unavailable in this build."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Object bounds and depth-assisted surface picking are unavailable."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Hide and isolate affect inspector helpers, not game rendering."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Raw source offsets and reference edges are not retained by the spawn catalog."});
}

} // namespace

bool SpawnInspectionProvider::refresh() {
    Key next{};
    activity::SessionSnapshot session{};
    server::bap::ActivitySnapshot privateActivity{};
    const bool privatePresent = server::bap::snapshot_private_activity(privateActivity);
    if (privatePresent) {
        next.sessionPresent = activity::snapshot_session(privateActivity.binding.sessionId, session);
        next.stale = !next.sessionPresent
                     || session.binding.sessionId != privateActivity.binding.sessionId
                     || session.binding.createdRevision != privateActivity.binding.createdRevision;
    } else {
        const std::uint64_t regionSession =
            activity::membership::live_region_session(activity::kAbsentSessionId);
        next.sessionPresent = regionSession != activity::kAbsentSessionId
                              && activity::snapshot_session(regionSession, session);
        next.stale = regionSession != activity::kAbsentSessionId && !next.sessionPresent;
    }

    if (next.sessionPresent) {
        next.activitySession = session.binding.sessionId;
        next.activityRevision = session.binding.createdRevision;
        next.activityIndex = session.binding.destination.activityIndex;
        next.region = session.reportedRegion;
        next.packageName = package_name(session.binding.destination);
        next.scenarioReady = state::build_data::scenario_layouts_ready();

        layouts::Definition layout{};
        next.scenarioPresent = next.scenarioReady && layouts::find(next.packageName, layout);
        if (next.scenarioPresent) {
            next.scenarioTag = layout.tag;
            next.mapStem = stem_name(layout);
            if (next.region >= 0) {
                const std::size_t bubble =
                    static_cast<std::size_t>(next.region) / tables::kSliceSetIndexFactor;
                if (bubble < layout.bubbleCount) {
                    next.bubble = static_cast<std::int32_t>(bubble);
                }
            }
        }

        const auto& destination = session.binding.destination;
        if (destination.hasSpawnSetOverride) {
            next.spawnSetHash = destination.spawnSetOverride;
        } else if (activity::destination::usable_spawn_set_hash(destination.hasSpawnSetHash,
                                                                 destination.spawnSetHash)) {
            next.spawnSetHash = destination.spawnSetHash;
        }
        next.spawnCatalogReady = state::build_data::spawn_sets_ready();
        next.stale = next.stale || !activity::binding_matches(session.binding);
    }

    if (keyPresent_ && next == key_) {
        return false;
    }
    key_ = next;
    keyPresent_ = true;
    if (generation_ == (std::numeric_limits<std::uint32_t>::max)()) {
        generation_ = 1;
    } else {
        ++generation_;
        if (generation_ == 0) {
            generation_ = 1;
        }
    }
    rebuild(next);
    return true;
}

void SpawnInspectionProvider::rebuild(const Key& key) {
    snapshot_ = {};
    snapshot_.graph.reset(generation_);
    snapshot_.packageName = key.packageName;
    snapshot_.mapStem = key.mapStem;
    snapshot_.activitySession = key.activitySession;
    snapshot_.activityRevision = key.activityRevision;
    snapshot_.scenarioTag = key.scenarioTag;
    snapshot_.spawnSetHash = key.spawnSetHash;
    snapshot_.activityIndex = key.activityIndex;
    snapshot_.region = key.region;
    snapshot_.sessionPresent = key.sessionPresent;
    snapshot_.scenarioPresent = key.scenarioPresent;
    snapshot_.spawnCatalogReady = key.spawnCatalogReady;
    snapshot_.stale = key.stale;
    if (key.bubble >= 0) {
        snapshot_.bubble = static_cast<std::uint16_t>(key.bubble);
    }

    Node root;
    root.name = key.packageName.empty() ? "World" : key.packageName;
    root.kind = NodeKind::world;
    root.status = key.stale ? Status::deferred
                           : (key.sessionPresent ? Status::known : Status::unknownSemantic);
    root.source.packageName = key.packageName;
    root.source.mapStem = key.mapStem;
    if (key.scenarioTag != 0) {
        root.tag = key.scenarioTag;
        root.source.scenarioTag = key.scenarioTag;
        root.actions = Action::copyId | Action::copyTag;
    } else {
        root.actions = Action::copyId;
    }
    if (key.activitySession != 0) {
        root.source.activitySession = key.activitySession;
    }
    if (key.activityIndex >= 0) {
        root.source.activityIndex = key.activityIndex;
    }
    if (key.bubble >= 0) {
        root.source.bubble = static_cast<std::uint16_t>(key.bubble);
    }
    const NodeId rootId = snapshot_.graph.add(std::move(root));

    add_capability_diagnostics(snapshot_.diagnostics);
    if (!key.sessionPresent) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "No committed activity session is available."});
        return;
    }
    if (key.stale) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The activity binding changed while it was sampled."});
    }
    if (!key.scenarioReady) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The scenario catalog is not ready."});
        return;
    }
    if (!key.scenarioPresent) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "No scenario layout matches the current package."});
        return;
    }

    layouts::Definition layout{};
    if (!layouts::find(key.packageName, layout)) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current scenario changed during graph rebuild."});
        return;
    }
    if (layout.truncated != 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The scenario bubble list is capacity-limited."});
    }
    if (key.bubble >= 0 && static_cast<std::size_t>(key.bubble) < layout.bubbleCount) {
        snapshot_.mapBubble = layout.bubbleMapIndices[static_cast<std::size_t>(key.bubble)];
    }
    if (key.mapStem.empty()) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current scenario has no map-package stem."});
        return;
    }
    if (key.spawnSetHash == 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current destination has no usable spawn-set hash."});
        return;
    }
    if (!key.spawnCatalogReady) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The spawn catalog is not ready."});
        return;
    }

    spawn_sets::NameHash set{};
    if (!spawn_sets::find_hash(key.mapStem, key.spawnSetHash, set)) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The current spawn-set hash is absent from this map."});
        return;
    }
    if (set.activityPackageOverflow != 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The spawn set's activity-package list is truncated."});
    }

    Node setNode;
    setNode.name = hex_label("Spawn set", key.spawnSetHash);
    setNode.kind = NodeKind::spawnSet;
    setNode.status = Status::known;
    setNode.nameHash = key.spawnSetHash;
    setNode.source = snapshot_.graph.node(rootId)->source;
    setNode.source.spawnSetHash = key.spawnSetHash;
    setNode.actions = Action::copyId;
    snapshot_.spawnSetNode = snapshot_.graph.add(std::move(setNode), rootId);

    const std::size_t pointCount = spawn_sets::point_count();
    std::vector<spawn_sets::Point> points(pointCount);
    std::size_t copied = 0;
    if (!spawn_sets::snapshot_points(points, copied) || copied != pointCount) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::error, "The spawn point bank could not be copied coherently."});
        return;
    }

    std::size_t ordinal = 0;
    for (std::size_t index = 0; index < copied; ++index) {
        const spawn_sets::Point& point = points[index];
        if (point.stemIndex != set.stemIndex || point.nameHash != key.spawnSetHash) {
            continue;
        }

        Node node;
        node.name = spawn_label(ordinal);
        node.kind = NodeKind::spawnPoint;
        node.status = Status::known;
        node.nameHash = point.nameHash;
        node.transform = Transform{point.position};
        node.source = snapshot_.graph.node(snapshot_.spawnSetNode)->source;
        node.actions = Action::focus | Action::hide | Action::isolate | Action::copyId
                       | Action::copyPosition;
        if (!snapshot_.graph.add(std::move(node), snapshot_.spawnSetNode)) {
            snapshot_.diagnostics.push_back(
                {Diagnostic::Severity::error, "The inspection graph reached its node-id capacity."});
            break;
        }
        ++ordinal;
    }

    if (ordinal == 0) {
        snapshot_.diagnostics.push_back(
            {Diagnostic::Severity::warning, "The selected spawn set contains no copied points."});
    }
}

const WorldSnapshot& SpawnInspectionProvider::snapshot() const noexcept {
    return snapshot_;
}

void SpawnInspectionProvider::reset() noexcept {
    snapshot_ = {};
    key_ = {};
    generation_ = 0;
    keyPresent_ = false;
}

} // namespace sunrise::client::inspection::providers
