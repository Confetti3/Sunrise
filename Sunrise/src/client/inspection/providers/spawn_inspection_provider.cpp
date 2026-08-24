#include "spawn_inspection_provider.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
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
namespace activity_logic = ::sunrise::client::inspection::providers::activity_logic;
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
    const int written = std::snprintf(text.data(),
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
    const int written = std::snprintf(text.data(), text.size(), "Destination %d", activityIndex);
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
         "An optional Activity Logic catalog can expose authored squad spawn rules, squads, "
         "triggers, "
         "areas/fields, objectives, devices, objects, actions, and conditions. Exact WorldID "
         "placement "
         "links are spatial helpers; they do not prove a live enemy or active encounter state."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Live runtime coverage proves the local controlled-object handle and "
                           "its published physics "
                           "position when the player observer is ready."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "The primary Wwise listener and bounded Havok body slots are copied from their existing "
         "runtime hooks. Physics slots are observations, not durable object identities."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "The engine's occupied object-datum iterator now provides general live object identities "
         "and object-system types. StaticMesh and Speedtree objects populate Geometry."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Terrain surfaces, trigger volume bounds, audio emitters, lights, "
                           "physics controllers, and "
                           "stable physics-body identities still need separate native producers."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Object transforms, bounds, and depth-assisted surface picking are unavailable for "
         "package-backed placement nodes."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Hide and isolate affect inspector helpers, not game rendering."});
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Raw source offsets and serialized reference edges are not retained by the current "
         "catalogs."});
}

void add_world_context_diagnostics(InspectionDocument& document,
                                   std::uint32_t localPlayerHandle,
                                   bool localPlayerPresent,
                                   std::size_t placedObjectCount,
                                   std::uint32_t runtimeObjectDeclaredCount,
                                   std::uint16_t runtimeObjectCount,
                                   bool audioListenerPresent,
                                   std::uint32_t physicsDeclaredSlots,
                                   std::uint16_t physicsBodyCount,
                                   bool scenarioCatalogReady,
                                   bool spawnCatalogReady) {
    const WorldContext& context = document.context;
    const int bubble = context.bubble ? static_cast<int>(*context.bubble) : -1;
    const int mapBubble = context.mapBubble ? static_cast<int>(*context.mapBubble) : -1;
    const char* state =
        context.stale ? "stale/deferred" : (context.sessionPresent ? "current" : "unavailable");

    std::array<char, 704> text{};
    const int written = std::snprintf(
        text.data(),
        text.size(),
        "World context: package='%s' map='%s' session=0x%016llX revision=%llu activity=%d "
        "region=%d bubble=%d mapBubble=%d scenario=0x%08X spawnSet=0x%08X "
        "localPlayer=0x%08X localPlayerPresent=%s placedObjects=%zu runtimeObjects=%u/%u "
        "audioListener=%s "
        "physicsBodies=%u/%u state=%s.",
        context.packageName.c_str(),
        context.mapStem.c_str(),
        static_cast<unsigned long long>(context.activitySession),
        static_cast<unsigned long long>(context.activityRevision),
        context.activityIndex,
        context.region,
        bubble,
        mapBubble,
        context.scenarioTag,
        context.spawnSetHash,
        localPlayerHandle,
        localPlayerPresent ? "yes" : "no",
        placedObjectCount,
        static_cast<unsigned>(runtimeObjectCount),
        runtimeObjectDeclaredCount,
        audioListenerPresent ? "yes" : "no",
        static_cast<unsigned>(physicsBodyCount),
        physicsDeclaredSlots,
        state);
    if (written > 0 && static_cast<std::size_t>(written) < text.size()) {
        document.diagnostics.push_back(
            {Diagnostic::Severity::information,
             std::string(text.data(), static_cast<std::size_t>(written))});
    }

    document.diagnostics.push_back({Diagnostic::Severity::information,
                                    std::string("Catalog readiness: scenario=")
                                        + (scenarioCatalogReady ? "ready" : "not ready")
                                        + ", spawns=" + (spawnCatalogReady ? "ready" : "not ready")
                                        + "."});
}

ProviderReport* find_report(InspectionDocument& document, Producer producer) noexcept {
    for (ProviderReport& report : document.providerReports) {
        if (report.producer == producer) {
            return &report;
        }
    }
    return nullptr;
}

void publish_report(InspectionDocument& document,
                    Producer producer,
                    std::uint64_t sequence,
                    std::uint64_t declaredCount,
                    std::uint64_t copiedCount,
                    std::uint32_t epoch,
                    bool installed,
                    bool ready,
                    bool truncated,
                    std::string failure = {}) {
    ProviderReport report{producer,
                          std::move(failure),
                          sequence,
                          declaredCount,
                          copiedCount,
                          epoch == 0 ? 1 : epoch,
                          installed,
                          ready,
                          truncated};
    if (ProviderReport* current = find_report(document, producer)) {
        *current = std::move(report);
    } else {
        document.providerReports.push_back(std::move(report));
    }
}

template <typename Value>
void publish_property(Graph& graph,
                      NodeId nodeId,
                      std::string key,
                      std::string label,
                      Value value,
                      Provenance provenance = Provenance::derived) {
    if (Node* node = graph.node(nodeId)) {
        node->properties.push_back(
            {std::move(key), std::move(label), PropertyValue{std::move(value)}, provenance});
    }
}

} // namespace

RefreshResult SpawnInspectionProvider::refresh() {
    Key next{};
    const live::Snapshot livePlayer = live::capture();
    const observed::ObjectSnapshot objects = observed::capture_objects();
    const observed::TriggerSnapshot triggers = observed::capture_triggers();
    const observed::AudioSnapshot audio = observed::capture_audio();
    const observed::PhysicsSnapshot physics = observed::capture_physics();
    next.localPlayerPresent = livePlayer.handlePresent;
    next.localPlayerPositionPresent = livePlayer.positionPresent;
    next.runtimeObjectsPresent = objects.present;
    next.runtimeObjectsTruncated = objects.truncated;
    next.runtimeObjectDeclaredCount = objects.declaredCount;
    next.runtimeObjectCount = objects.objectCount;
    for (std::size_t index = 0; index < objects.objectCount; ++index) {
        next.runtimeObjectHandles[index] = objects.objects[index].handle;
        next.runtimeObjectTypes[index] = objects.objects[index].type;
    }
    next.triggersPresent = triggers.present;
    next.triggersTruncated = triggers.truncated;
    next.triggerCount = triggers.triggerCount;
    for (std::size_t index = 0; index < triggers.triggerCount; ++index) {
        next.triggerIdentities[index] = observed::trigger_identity(triggers.triggers[index]);
    }
    if (livePlayer.handlePresent) {
        next.localPlayerHandle = livePlayer.controlledHandle;
    }
    next.audioListenerPresent = audio.present;
    next.physicsPresent = physics.present;
    next.physicsDeclaredSlots = physics.declaredSlots;
    next.physicsBodyCount = physics.bodyCount;
    next.physicsTruncated = physics.truncated;
    for (std::size_t index = 0; index < physics.bodyCount; ++index) {
        next.physicsSlots[index] = physics.bodies[index].slot;
    }
    activity::SessionSnapshot session{};
    server::bap::ActivitySnapshot privateActivity{};
    next.activityLogicBrowseScenarioTag = browseScenarioTag_;
    const bool privatePresent = server::bap::snapshot_private_activity(privateActivity);
    if (privatePresent) {
        next.sessionPresent =
            activity::snapshot_session(privateActivity.binding.sessionId, session);
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
            next.scenarioTruncated = layout.truncated != 0;
            if (next.region >= 0) {
                const std::size_t bubble =
                    static_cast<std::size_t>(next.region) / tables::kSliceSetIndexFactor;
                if (bubble < layout.bubbleCount) {
                    next.bubble = static_cast<std::int32_t>(bubble);
                    next.mapBubble = static_cast<std::int32_t>(layout.bubbleMapIndices[bubble]);
                }
            }
            const PlacedCacheKey cacheKey{next.packageName,
                                          next.mapStem,
                                          next.activitySession,
                                          next.activityRevision,
                                          next.scenarioTag,
                                          next.bubble};
            if (!placedCachePresent_ || cacheKey != placedCacheKey_) {
                placedCache_.clear();
                placed::collect(layout, next.bubble, placedCache_);
                placedCacheKey_ = cacheKey;
                placedCachePresent_ = true;
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

    const auto position_matches =
        [](const Node* node, bool present, const std::array<float, 3>& position) noexcept {
            if (node == nullptr || node->transform.has_value() != present) {
                return false;
            }
            return !present || (node->transform->position == position && node->transformRuntime);
        };
    const auto runtime_values_changed = [&]() noexcept {
        bool changed = false;
        const Node* local = document_.graph.node(publishedNodes_.localPlayer);
        changed = changed
                  || (local != nullptr
                      && !position_matches(local, livePlayer.positionPresent, livePlayer.position));
        if (objects.present && publishedNodes_.runtimeObjects.size() == objects.objectCount) {
            for (std::size_t index = 0; index < objects.objectCount; ++index) {
                const Node* node = document_.graph.node(publishedNodes_.runtimeObjects[index]);
                const auto& observation = objects.objects[index];
                changed =
                    changed || node == nullptr || node->runtimeEntity != observation.handle
                    || !position_matches(node, observation.positionPresent, observation.position);
            }
        }
        if (triggers.present && publishedNodes_.triggers.size() == triggers.triggerCount) {
            for (std::size_t index = 0; index < triggers.triggerCount; ++index) {
                const Node* node = document_.graph.node(publishedNodes_.triggers[index]);
                const auto& observation = triggers.triggers[index];
                changed = changed || node == nullptr
                          || node->observationId != observed::trigger_identity(observation)
                          || node->triggerActive != std::optional<bool>{observation.active}
                          || node->triggerEnabled != std::optional<bool>{observation.enabled};
                if (observation.kind == client::viewer::triggers::Kind::volume) {
                    changed = changed
                              || node->triggerOverlapCount
                                     != std::optional<std::uint32_t>{observation.overlapCount}
                              || !position_matches(
                                  node, observation.positionPresent, observation.position);
                } else {
                    changed = changed
                              || node->triggerSelector
                                     != std::optional<std::int32_t>{observation.selector}
                              || node->triggerSourceHash
                                     != std::optional<std::uint32_t>{observation.sourceHash};
                }
            }
        }
        if (audio.present) {
            changed = changed
                      || !position_matches(document_.graph.node(publishedNodes_.audioListener),
                                           true,
                                           audio.position);
        }
        if (physics.present && publishedNodes_.physicsBodies.size() == physics.bodyCount) {
            for (std::size_t index = 0; index < physics.bodyCount; ++index) {
                const Node* node = document_.graph.node(publishedNodes_.physicsBodies[index]);
                const auto& observation = physics.bodies[index];
                changed = changed || node == nullptr
                          || !position_matches(node, true, observation.position)
                          || node->linearVelocity
                                 != std::optional<std::array<float, 3>>{observation.velocity};
            }
        }
        return changed;
    };

    auto advance_value_revision = [this]() noexcept {
        if (valueRevision_ == (std::numeric_limits<std::uint64_t>::max)()) {
            valueRevision_ = 1;
        } else {
            ++valueRevision_;
            if (valueRevision_ == 0) {
                valueRevision_ = 1;
            }
        }
    };

    if (keyPresent_ && next == key_) {
        const bool valueChanged = runtime_values_changed();
        live::update(document_.graph, publishedNodes_.localPlayer, livePlayer);
        observed::update_objects(document_.graph, publishedNodes_.runtimeObjects, objects);
        observed::update_triggers(document_.graph, publishedNodes_.triggers, triggers, objects);
        observed::update_audio(document_.graph, publishedNodes_.audioListener, audio);
        observed::update_physics(document_.graph, publishedNodes_.physicsBodies, physics);
        if (!valueChanged) {
            synchronize_document(RefreshKind::none, objects, triggers, physics);
            return RefreshResult{RefreshKind::none, valueRevision_};
        }
        advance_value_revision();
        synchronize_document(RefreshKind::value, objects, triggers, physics);
        return RefreshResult{RefreshKind::value, valueRevision_};
    }
    Key structuralNext = next;
    Key structuralCurrent = key_;
    // Runtime identity arrays and bounded counts are structural membership.  Dynamic
    // positions are intentionally not part of Key, so a moving object stays on the
    // existing graph while a membership change rebuilds it safely.
    const bool producerReset = !keyPresent_ || next.activitySession != key_.activitySession
                               || next.activityRevision != key_.activityRevision;
    const bool structuralChanged = !keyPresent_ || structuralNext != structuralCurrent;
    if (producerReset) {
        client::viewer::objects::reset_activity();
        ++producerEpoch_;
        if (producerEpoch_ == 0) {
            producerEpoch_ = 1;
        }
    }
    key_ = next;
    keyPresent_ = true;
    if (structuralChanged) {
        if (generation_ == (std::numeric_limits<std::uint32_t>::max)()) {
            generation_ = 1;
        } else {
            ++generation_;
            if (generation_ == 0) {
                generation_ = 1;
            }
        }
    }
    if (!structuralChanged) {
        const bool valueChanged = runtime_values_changed();
        live::update(document_.graph, publishedNodes_.localPlayer, livePlayer);
        observed::update_objects(document_.graph, publishedNodes_.runtimeObjects, objects);
        observed::update_triggers(document_.graph, publishedNodes_.triggers, triggers, objects);
        observed::update_audio(document_.graph, publishedNodes_.audioListener, audio);
        observed::update_physics(document_.graph, publishedNodes_.physicsBodies, physics);
        if (!valueChanged) {
            synchronize_document(RefreshKind::none, objects, triggers, physics);
            return RefreshResult{RefreshKind::none, valueRevision_};
        }
        advance_value_revision();
        key_ = next;
        synchronize_document(RefreshKind::value, objects, triggers, physics);
        return RefreshResult{RefreshKind::value, valueRevision_};
    }
    advance_value_revision();
    rebuild(next, livePlayer, objects, triggers, audio, physics);
    synchronize_document(RefreshKind::structural, objects, triggers, physics);
    return RefreshResult{RefreshKind::structural, valueRevision_};
}

void SpawnInspectionProvider::rebuild(const Key& key,
                                      const live::Snapshot& livePlayer,
                                      const observed::ObjectSnapshot& objects,
                                      const observed::TriggerSnapshot& triggers,
                                      const observed::AudioSnapshot& audio,
                                      const observed::PhysicsSnapshot& physics) {
    document_ = {};
    publishedNodes_ = {};
    const std::uint32_t epoch = producerEpoch_ == 0 ? 1 : producerEpoch_;
    document_.graph.reset(generation_, epoch);
    document_.context.packageName = key.packageName;
    document_.context.mapStem = key.mapStem;
    document_.context.activitySession = key.activitySession;
    document_.context.activityRevision = key.activityRevision;
    document_.context.scenarioTag = key.scenarioTag;
    document_.context.spawnSetHash = key.spawnSetHash;
    document_.context.activityIndex = key.activityIndex;
    document_.context.region = key.region;
    document_.context.sessionPresent = key.sessionPresent;
    document_.context.stale = key.stale;
    if (key.bubble >= 0) {
        document_.context.bubble = static_cast<std::uint16_t>(key.bubble);
    }
    if (key.mapBubble >= 0) {
        document_.context.mapBubble = static_cast<std::uint16_t>(key.mapBubble);
    }

    Source source;
    source.packageName = key.packageName;
    source.mapStem = key.mapStem;
    if (key.scenarioTag != 0) {
        source.scenarioTag = key.scenarioTag;
    }
    if (key.spawnSetHash != 0) {
        source.spawnSetHash = key.spawnSetHash;
    }
    if (key.activitySession != 0) {
        source.activitySession = key.activitySession;
    }
    if (key.activityIndex >= 0) {
        source.activityIndex = key.activityIndex;
    }
    if (key.bubble >= 0) {
        source.bubble = static_cast<std::uint16_t>(key.bubble);
    }

    Node root;
    root.name = key.packageName.empty() ? "World" : key.packageName;
    root.kind = NodeKind::world;
    root.producer = Producer::graph;
    root.provenance = Provenance::derived;
    root.nativeKey = key.activitySession;
    root.status = key.stale ? Status::deferred
                            : (key.sessionPresent ? Status::known : Status::unknownSemantic);
    root.source = source;
    root.actions = Action::copyId;
    const NodeId rootId = document_.graph.add(std::move(root));
    if (!rootId) {
        document_.diagnostics.push_back(
            {Diagnostic::Severity::error, "The inspection graph could not create its world root."});
        return;
    }

    NodeId graphParent = rootId;

    if (key.sessionPresent) {
        Node activityNode;
        activityNode.name = session_label(key.activitySession);
        activityNode.kind = NodeKind::activity;
        activityNode.producer = Producer::graph;
        activityNode.provenance = Provenance::derived;
        activityNode.nativeKey = key.activitySession;
        activityNode.status = key.stale ? Status::deferred : Status::known;
        activityNode.source = source;
        activityNode.actions = Action::copyId;
        const NodeId activityId = document_.graph.add(std::move(activityNode), graphParent);
        if (activityId) {
            graphParent = activityId;
        }

        Node destinationNode;
        destinationNode.name = destination_label(key.activityIndex);
        destinationNode.kind = NodeKind::destination;
        destinationNode.producer = Producer::catalog;
        destinationNode.provenance = Provenance::catalog;
        destinationNode.nativeKey = static_cast<std::uint32_t>(key.activityIndex);
        destinationNode.status = key.stale ? Status::deferred : Status::known;
        destinationNode.source = source;
        destinationNode.actions = Action::copyId;
        const NodeId destinationId = document_.graph.add(std::move(destinationNode), graphParent);
        if (destinationId) {
            graphParent = destinationId;
        }
    }

    if (key.scenarioReady || key.scenarioPresent || key.scenarioTag != 0 || !key.mapStem.empty()) {
        Node scenarioNode;
        scenarioNode.name =
            key.scenarioTag != 0 ? hex_label("Scenario", key.scenarioTag) : std::string("Scenario");
        scenarioNode.kind = NodeKind::source;
        scenarioNode.producer = Producer::catalog;
        scenarioNode.provenance = Provenance::catalog;
        scenarioNode.nativeKey = key.scenarioTag;
        scenarioNode.status = !key.scenarioReady
                                  ? Status::deferred
                                  : (key.scenarioPresent ? Status::known : Status::unknownSemantic);
        scenarioNode.source = source;
        if (key.scenarioTag != 0) {
            scenarioNode.tag = key.scenarioTag;
            scenarioNode.actions = Action::copyId | Action::copyTag;
        } else {
            scenarioNode.actions = Action::copyId;
        }
        const NodeId scenarioId = document_.graph.add(std::move(scenarioNode), graphParent);
        if (scenarioId) {
            graphParent = scenarioId;
        }
    }

    const live::AppendResult liveResult =
        live::append(document_.graph, document_.diagnostics, livePlayer, source, graphParent);
    publishedNodes_.localPlayer = liveResult.node;

    const placed::AppendResult placedResult =
        placed::append(document_.graph, document_.diagnostics, placedCache_, source, graphParent);
    add_capability_diagnostics(document_.diagnostics);
    add_world_context_diagnostics(document_,
                                  key.localPlayerHandle,
                                  key.localPlayerPresent,
                                  placedResult.objectCount,
                                  key.runtimeObjectDeclaredCount,
                                  key.runtimeObjectCount,
                                  key.audioListenerPresent,
                                  key.physicsDeclaredSlots,
                                  key.physicsBodyCount,
                                  key.scenarioReady,
                                  key.spawnCatalogReady);

    NodeId spawnSetNode{};
    std::size_t spawnPointCount{};
    [&]() {
        if (!key.sessionPresent) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning, "No committed activity session is available."});
            return;
        }
        if (key.stale) {
            document_.diagnostics.push_back({Diagnostic::Severity::warning,
                                             "The activity binding changed while it was sampled."});
        }
        if (!key.scenarioReady) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning, "The scenario catalog is not ready."});
            return;
        }
        if (!key.scenarioPresent) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning, "No scenario layout matches the current package."});
            return;
        }
        if (key.scenarioTruncated) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning, "The scenario bubble list is capacity-limited."});
        }
        if (key.mapStem.empty()) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning, "The current scenario has no map-package stem."});
            return;
        }
        if (key.spawnSetHash == 0) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning,
                 "The current destination has no usable spawn-set hash."});
            return;
        }
        if (!key.spawnCatalogReady) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning, "The spawn catalog is not ready."});
            return;
        }

        spawn_sets::NameHash set{};
        if (!spawn_sets::find_hash(key.mapStem, key.spawnSetHash, set)) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning,
                 "The current spawn-set hash is absent from this map."});
            return;
        }
        if (set.activityPackageOverflow != 0) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::warning,
                 "The spawn set's activity-package list is truncated."});
        }

        Node setNode;
        setNode.name = hex_label("Spawn set", key.spawnSetHash);
        setNode.kind = NodeKind::spawnSet;
        setNode.producer = Producer::catalog;
        setNode.provenance = Provenance::catalog;
        setNode.nativeKey = key.spawnSetHash;
        setNode.status = Status::known;
        setNode.nameHash = key.spawnSetHash;
        setNode.source = source;
        setNode.source.spawnSetHash = key.spawnSetHash;
        setNode.actions = Action::copyId;
        spawnSetNode = document_.graph.add(std::move(setNode), graphParent);
        if (!spawnSetNode) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph could not create the spawn set."});
            return;
        }

        const std::size_t pointCount = spawn_sets::point_count();
        std::vector<spawn_sets::Point> points(pointCount);
        std::size_t copied = 0;
        if (!spawn_sets::snapshot_points(points, copied) || copied != pointCount) {
            document_.diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The spawn point bank could not be copied coherently."});
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
            node.producer = Producer::catalog;
            node.provenance = Provenance::catalog;
            node.nativeKey = (static_cast<std::uint64_t>(point.nameHash) << 32U)
                             ^ static_cast<std::uint64_t>(ordinal + 1U);
            node.status = Status::known;
            node.nameHash = point.nameHash;
            node.transform = Transform{point.position};
            bool rotationFinite = true;
            for (const float lane : point.rotation) {
                rotationFinite = rotationFinite && std::isfinite(lane);
            }
            if (rotationFinite) {
                node.authoredRotation = point.rotation;
            }
            node.source = source;
            node.source.spawnSetHash = key.spawnSetHash;
            node.actions = Action::focus | Action::hide | Action::isolate | Action::copyId
                           | Action::copyPosition;
            if (!document_.graph.add(std::move(node), spawnSetNode)) {
                document_.diagnostics.push_back(
                    {Diagnostic::Severity::error,
                     "The inspection graph reached its node-id capacity."});
                break;
            }
            ++ordinal;
        }

        if (ordinal == 0) {
            document_.diagnostics.push_back({Diagnostic::Severity::warning,
                                             "The selected spawn set contains no copied points."});
        }
        spawnPointCount = ordinal;
    }();

    const observed::AppendResult observedResult = observed::append(document_.graph,
                                                                   document_.diagnostics,
                                                                   objects,
                                                                   triggers,
                                                                   audio,
                                                                   physics,
                                                                   source,
                                                                   graphParent);
    publishedNodes_.runtimeObjects = observedResult.objects;
    publishedNodes_.triggers = observedResult.triggers;
    publishedNodes_.audioListener = observedResult.audioListener;
    publishedNodes_.physicsBodies = observedResult.physicsBodies;
    const activity_graph::AppendResult activityResult =
        activity_graph::append(document_.graph, document_.diagnostics, source, graphParent);

    const bubble_bounds::AppendResult bubbleResult =
        bubble_bounds::append(document_.graph, document_.diagnostics, source, graphParent);

    const statics_footprints::AppendResult staticsResult =
        statics_footprints::append(document_.graph, document_.diagnostics, source, graphParent);

    const activity_logic::AppendResult logicResult =
        key.activityLogicBrowseScenarioTag != 0
            ? activity_logic::append_browse(document_.graph,
                                            document_.diagnostics,
                                            key.activityLogicBrowseScenarioTag,
                                            graphParent)
            : activity_logic::append(document_.graph, document_.diagnostics, source, graphParent);

    publish_property(document_.graph,
                     rootId,
                     "placed_object_count",
                     "Placed objects",
                     static_cast<std::uint64_t>(placedResult.objectCount));
    publish_property(document_.graph,
                     rootId,
                     "placed_component_slot_count",
                     "Placed component slots",
                     static_cast<std::uint64_t>(placedResult.slotCount));
    publish_property(document_.graph,
                     rootId,
                     "activity_catalog_build",
                     "Activity catalog build",
                     static_cast<std::uint64_t>(activityResult.contentBuild),
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_catalog_version",
                     "Activity catalog version",
                     activityResult.version,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_catalog_build_match",
                     "Activity catalog matches build",
                     activityResult.buildMatch,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_catalog_diagnostic",
                     "Activity catalog status",
                     activityResult.diagnostic,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_activity",
                     "Activity logic activity",
                     logicResult.activityName,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_destination",
                     "Activity logic destination",
                     logicResult.destination,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_browse_only",
                     "Activity logic browse only",
                     logicResult.browseOnly,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_browse_scenario",
                     "Activity logic browse scenario",
                     static_cast<std::uint64_t>(key.activityLogicBrowseScenarioTag),
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_matched",
                     "Activity logic matches world",
                     logicResult.matched,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_diagnostic",
                     "Activity logic status",
                     logicResult.diagnostic,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_definition_count",
                     "Activity logic definitions",
                     static_cast<std::uint64_t>(logicResult.definitionCount),
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "activity_logic_placement_count",
                     "Activity logic placements",
                     static_cast<std::uint64_t>(logicResult.placementCount),
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "bubble_bounds_build",
                     "Bubble bounds build",
                     static_cast<std::uint64_t>(bubbleResult.contentBuild),
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "bubble_bounds_build_match",
                     "Bubble bounds match build",
                     bubbleResult.buildMatch,
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "bubble_bounds_count",
                     "Bubble bounds",
                     static_cast<std::uint64_t>(bubbleResult.bubbleCount),
                     Provenance::catalog);
    publish_property(document_.graph,
                     rootId,
                     "static_footprint_count",
                     "Static footprints",
                     static_cast<std::uint64_t>(staticsResult.published),
                     Provenance::catalog);

    document_.providerReports.reserve(12);
    publish_report(document_,
                   Producer::worldContext,
                   valueRevision_,
                   key.sessionPresent ? 1 : 0,
                   key.sessionPresent ? 1 : 0,
                   epoch,
                   true,
                   key.sessionPresent && !key.stale,
                   false,
                   key.stale ? "copied context is stale" : std::string{});
    publish_report(document_,
                   Producer::placedContent,
                   valueRevision_,
                   placedResult.objectCount + placedResult.slotCount,
                   placedResult.objectCount + placedResult.slotCount,
                   epoch,
                   true,
                   key.scenarioReady,
                   key.scenarioTruncated || placedResult.slotsTruncated);
    publish_report(document_,
                   Producer::spawnPoints,
                   valueRevision_,
                   spawnPointCount,
                   spawnPointCount,
                   epoch,
                   key.spawnCatalogReady,
                   static_cast<bool>(spawnSetNode),
                   false);
    publish_report(document_,
                   Producer::activityCatalog,
                   valueRevision_,
                   activityResult.present ? 1 : 0,
                   activityResult.present ? 1 : 0,
                   epoch,
                   activityResult.present,
                   activityResult.buildMatch,
                   false,
                   activityResult.diagnostic);
    publish_report(document_,
                   Producer::activityLogicCatalog,
                   valueRevision_,
                   logicResult.definitionCount,
                   logicResult.definitionCount,
                   epoch,
                   logicResult.present,
                   logicResult.matched || logicResult.browseOnly,
                   false,
                   logicResult.diagnostic);
    publish_report(document_,
                   Producer::bubbleBounds,
                   valueRevision_,
                   bubbleResult.bubbleCount,
                   bubbleResult.bubbleCount,
                   epoch,
                   bubbleResult.present,
                   bubbleResult.buildMatch,
                   false,
                   bubbleResult.diagnostic);
    publish_report(document_,
                   Producer::staticFootprints,
                   valueRevision_,
                   staticsResult.published,
                   staticsResult.published,
                   epoch,
                   true,
                   static_cast<bool>(staticsResult.groupNode),
                   false);
    publish_report(document_,
                   Producer::localPlayer,
                   valueRevision_,
                   key.localPlayerPresent ? 1 : 0,
                   key.localPlayerPresent ? 1 : 0,
                   epoch,
                   true,
                   key.localPlayerPresent,
                   false);
}

void SpawnInspectionProvider::synchronize_document(
    RefreshKind kind,
    const observed::ObjectSnapshot& objects,
    const observed::TriggerSnapshot& triggers,
    const observed::PhysicsSnapshot& physics) noexcept {
    const std::uint32_t epoch = producerEpoch_ == 0 ? 1 : producerEpoch_;
    document_.structureRevision = document_.graph.generation();
    document_.valueRevision = valueRevision_;

    const bool objectsInstalled = client::viewer::objects::installed();
    publish_report(document_,
                   Producer::objectSystem,
                   objects.sequence,
                   key_.runtimeObjectDeclaredCount,
                   key_.runtimeObjectCount,
                   epoch,
                   objectsInstalled,
                   key_.runtimeObjectsPresent,
                   key_.runtimeObjectsTruncated,
                   objectsInstalled ? std::string{} : "not installed");
    const bool triggersInstalled = client::viewer::triggers::installed();
    publish_report(document_,
                   Producer::trigger,
                   triggers.sequence,
                   key_.triggerCount,
                   key_.triggerCount,
                   epoch,
                   triggersInstalled,
                   key_.triggersPresent,
                   key_.triggersTruncated,
                   triggersInstalled ? std::string{} : "not installed");
    const bool audioInstalled = client::viewer::audio::installed();
    publish_report(document_,
                   Producer::audioListener,
                   valueRevision_,
                   key_.audioListenerPresent ? 1 : 0,
                   key_.audioListenerPresent ? 1 : 0,
                   epoch,
                   audioInstalled,
                   key_.audioListenerPresent,
                   false,
                   audioInstalled ? std::string{} : "not installed");
    const bool physicsInstalled = client::hooks::noclip::installed();
    publish_report(document_,
                   Producer::physics,
                   physics.sequence,
                   key_.physicsDeclaredSlots,
                   key_.physicsBodyCount,
                   epoch,
                   physicsInstalled,
                   key_.physicsPresent,
                   key_.physicsTruncated,
                   physicsInstalled ? std::string{} : "not installed");
    if (kind == RefreshKind::structural && document_.graph.rejected_duplicate_keys() != 0) {
        document_.diagnostics.push_back(
            {Diagnostic::Severity::error,
             "One or more nodes were rejected because their stable keys collided."});
    }
}

const InspectionDocument& SpawnInspectionProvider::snapshot() const noexcept {
    return document_;
}

void SpawnInspectionProvider::set_activity_logic_browse(std::uint32_t scenarioTag) noexcept {
    browseScenarioTag_ = scenarioTag;
}

void SpawnInspectionProvider::reset() noexcept {
    document_ = {};
    publishedNodes_ = {};
    key_ = {};
    generation_ = 0;
    placedCacheKey_ = {};
    placedCache_.clear();
    placedCachePresent_ = false;
    browseScenarioTag_ = 0;
    ++producerEpoch_;
    if (producerEpoch_ == 0) {
        producerEpoch_ = 1;
    }
    keyPresent_ = false;
}

} // namespace sunrise::client::inspection::providers
