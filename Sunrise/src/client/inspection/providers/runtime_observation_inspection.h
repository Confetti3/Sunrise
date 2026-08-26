#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "../../hooks/noclip/runtime.h"
#include "../../hooks/viewer_audio/viewer_audio.h"
#include "../../hooks/viewer_objects/viewer_objects.h"
#include "../../hooks/viewer_triggers/viewer_triggers.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::runtime_observations {

namespace noclip = client::hooks::noclip;
namespace audio = client::viewer::audio;
namespace objects = client::viewer::objects;
namespace triggers = client::viewer::triggers;

struct AudioSnapshot final {
    std::array<float, 3> position{};
    bool present{};
};

struct PhysicsSnapshot final {
    std::array<noclip::PhysicsBodyObservation, noclip::kPhysicsObservationCapacity> bodies{};
    std::uint32_t declaredSlots{};
    std::uint64_t sequence{};
    std::uint16_t bodyCount{};
    bool present{};
    bool truncated{};
};

using ObjectSnapshot = objects::Snapshot;
using TriggerSnapshot = triggers::Snapshot;

struct AppendResult final {
    NodeId objectGroup{};
    std::vector<NodeId> objects;
    NodeId triggerGroup{};
    std::vector<NodeId> triggers;
    NodeId audioListener{};
    NodeId physicsGroup{};
    std::vector<NodeId> physicsBodies;
};

[[nodiscard]] inline TriggerSnapshot capture_triggers() noexcept {
    TriggerSnapshot current{};
    (void)triggers::snapshot(current);
    return current;
}

[[nodiscard]] inline ObjectSnapshot capture_objects() noexcept {
    ObjectSnapshot current{};
    (void)objects::snapshot(current);
    return current;
}

[[nodiscard]] inline const char* object_type_name(std::uint8_t type) noexcept {
    switch (type) {
    case 0x00:
        return "Inherited";
    case 0x01:
        return "Static mesh";
    case 0x02:
        return "Simple prop (deprecated)";
    case 0x03:
        return "Expensive prop (deprecated)";
    case 0x04:
        return "Cosmetic static prop";
    case 0x05:
        return "Cosmetic movable prop";
    case 0x06:
        return "Garbage movable prop";
    case 0x07:
        return "Networked static prop";
    case 0x08:
        return "Networked movable prop";
    case 0x09:
        return "Cinematic prop";
    case 0x0A:
        return "Speedtree";
    case 0x0B:
        return "Interactive object";
    case 0x0C:
        return "Biped";
    case 0x0D:
        return "Creature";
    case 0x0E:
        return "Weapon";
    case 0x0F:
        return "Vehicle";
    case 0x10:
        return "Turret";
    case 0x11:
        return "Emitter";
    case 0x12:
        return "Projectile";
    case 0x13:
        return "Item";
    case 0x14:
        return "Ammo item";
    case 0x15:
        return "Loot item";
    case 0x16:
        return "Gear";
    case 0x17:
        return "Hop-on";
    case 0x18:
        return "Hop-on gear biped";
    case 0x19:
        return "Hop-on gear weapon";
    case 0x1A:
        return "Hop-on gear ship";
    case 0x1B:
        return "Hop-on gear sparrow";
    case 0x1C:
        return "System object";
    case 0xFF:
        return "Invalid object";
    default:
        return "Unknown object type";
    }
}

[[nodiscard]] inline NodeKind object_kind(std::uint8_t type) noexcept {
    return type == 0x01 || type == 0x0A ? NodeKind::geometry : NodeKind::runtimeEntity;
}

[[nodiscard]] inline bool object_type_known(std::uint8_t type) noexcept {
    return type <= 0x1C;
}

[[nodiscard]] inline std::string object_label(const objects::Observation& observation) {
    std::array<char, 96> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "%s 0x%08X",
                                      object_type_name(observation.type),
                                      observation.handle);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Runtime object");
}

[[nodiscard]] inline bool valid_position(const std::array<float, 3>& position) noexcept {
    for (const float lane : position) {
        if (!std::isfinite(lane)) {
            return false;
        }
    }
    return true;
}

inline void
apply_runtime_position(Node& node, const std::array<float, 3>& position, bool present) noexcept {
    if (!present || !valid_position(position)) {
        node.transform.reset();
        node.transformRuntime = false;
        node.actions = Action::copyId;
        return;
    }
    node.transform = Transform{position};
    node.transformRuntime = true;
    node.actions = spatial_actions();
}

inline void append_objects(Graph& graph,
                           std::vector<Diagnostic>& diagnostics,
                           const ObjectSnapshot& snapshot,
                           const Source& source,
                           NodeId parent,
                           AppendResult& result) {
    if (!snapshot.present) {
        return;
    }
    Node group;
    std::array<char, 96> label{};
    const int written = std::snprintf(label.data(),
                                      label.size(),
                                      snapshot.truncated ? "Live object system (%u/at least %u)"
                                                         : "Live object system (%u/%u)",
                                      static_cast<unsigned>(snapshot.objectCount),
                                      snapshot.declaredCount);
    group.name = written > 0 && static_cast<std::size_t>(written) < label.size()
                     ? std::string(label.data(), static_cast<std::size_t>(written))
                     : std::string("Live object system");
    group.searchText = "live runtime object system official occupied datum iterator";
    group.kind = NodeKind::runtimeEntity;
    group.status = snapshot.truncated ? Status::deferred : Status::known;
    group.producer = Producer::objectSystem;
    group.provenance = Provenance::runtime;
    group.source = source;
    group.actions = Action::copyId;
    result.objectGroup = graph.add(std::move(group), parent);
    if (!result.objectGroup) {
        diagnostics.push_back(
            {Diagnostic::Severity::error,
             "The inspection graph could not create the live object-system group."});
        return;
    }
    result.objects.reserve(snapshot.objectCount);
    for (std::size_t index = 0; index < snapshot.objectCount; ++index) {
        const objects::Observation& observation = snapshot.objects[index];
        Node node;
        node.name = object_label(observation);
        node.searchText =
            std::string("live runtime object system ") + object_type_name(observation.type);
        node.kind = object_kind(observation.type);
        node.status = object_type_known(observation.type) ? Status::known : Status::unknownSemantic;
        node.producer = Producer::objectSystem;
        node.provenance = Provenance::runtime;
        node.nativeKey = observation.handle;
        node.source = source;
        node.runtimeEntity = observation.handle;
        node.objectSystemType = observation.type;
        apply_runtime_position(node, observation.position, observation.positionPresent);
        const NodeId id = graph.add(std::move(node), result.objectGroup);
        if (!id) {
            diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph reached its node-id capacity while adding live objects."});
            break;
        }
        result.objects.push_back(id);
    }
    diagnostics.push_back(
        {snapshot.truncated ? Diagnostic::Severity::warning : Diagnostic::Severity::information,
         snapshot.truncated
             ? "The live object-system iterator exceeded its bounded copy capacity."
             : "Live objects come from the engine's occupied-datum iterator; physics positions "
               "are attached only after that object is observed by the existing sync hook."});
}

[[nodiscard]] inline AudioSnapshot capture_audio() noexcept {
    audio::ListenerSnapshot current{};
    AudioSnapshot result{};
    result.present = audio::listener_snapshot(current);
    if (result.present) {
        result.position = current.position;
    }
    return result;
}

[[nodiscard]] inline PhysicsSnapshot capture_physics() noexcept {
    noclip::PhysicsObservationSnapshot current{};
    PhysicsSnapshot result{};
    result.present = noclip::physics_observation_snapshot(current);
    if (!result.present) {
        return result;
    }
    result.bodies = current.bodies;
    result.sequence = current.sequence;
    result.declaredSlots = current.declaredSlots;
    result.bodyCount = current.bodyCount;
    result.truncated = current.truncated;
    return result;
}

[[nodiscard]] inline std::string physics_label(std::uint64_t slot) {
    const bool inactive = (slot >> 63U) != 0;
    const std::uint32_t island = static_cast<std::uint32_t>((slot >> 32U) & 0x7FFFFFFFU);
    const std::uint32_t entity = static_cast<std::uint32_t>(slot);
    std::array<char, 96> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "Havok %s slot %u:%u",
                                      inactive ? "inactive" : "active",
                                      island,
                                      entity);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Havok body slot");
}

inline void append_audio(Graph& graph,
                         std::vector<Diagnostic>& diagnostics,
                         const AudioSnapshot& snapshot,
                         const Source& source,
                         NodeId parent,
                         AppendResult& result) {
    if (!snapshot.present) {
        return;
    }
    Node node;
    node.name = "Primary Wwise listener 0";
    node.searchText = "runtime audio primary wwise listener copied effective position";
    node.kind = NodeKind::audio;
    node.status = Status::known;
    node.producer = Producer::audioListener;
    node.provenance = Provenance::runtime;
    node.source = source;
    node.observationId = 0;
    node.nativeKey = 1;
    apply_runtime_position(node, snapshot.position, snapshot.present);
    result.audioListener = graph.add(std::move(node), parent);
    if (!result.audioListener) {
        diagnostics.push_back(
            {Diagnostic::Severity::error,
             "The inspection graph could not create the primary-listener observation."});
    }
}

[[nodiscard]] inline std::uint64_t trigger_identity(const triggers::Observation& observation) {
    return observation.observationId;
}

inline void append_triggers(Graph& graph,
                            std::vector<Diagnostic>& diagnostics,
                            const TriggerSnapshot& snapshot,
                            const ObjectSnapshot& objectsSnapshot,
                            const Source& source,
                            NodeId parent,
                            AppendResult& result) {
    if (!snapshot.present || snapshot.triggerCount == 0) {
        return;
    }
    Node group;
    std::array<char, 96> groupLabel{};
    const int groupWritten = std::snprintf(groupLabel.data(),
                                           groupLabel.size(),
                                           "Native trigger events (%u)",
                                           static_cast<unsigned>(snapshot.triggerCount));
    group.name = groupWritten > 0 && static_cast<std::size_t>(groupWritten) < groupLabel.size()
                     ? std::string(groupLabel.data(), static_cast<std::size_t>(groupWritten))
                     : std::string("Native trigger events");
    group.searchText = "runtime native trigger event component";
    group.kind = NodeKind::trigger;
    group.status = snapshot.truncated ? Status::deferred : Status::known;
    group.producer = Producer::trigger;
    group.provenance = Provenance::runtime;
    group.source = source;
    group.actions = Action::copyId;
    result.triggerGroup = graph.add(std::move(group), parent);
    if (!result.triggerGroup) {
        diagnostics.push_back({Diagnostic::Severity::error,
                               "The inspection graph could not create the trigger group."});
        return;
    }

    result.triggers.reserve(snapshot.triggerCount);
    for (std::size_t index = 0; index < snapshot.triggerCount; ++index) {
        const triggers::Observation& observation = snapshot.triggers[index];
        Node node;
        std::array<char, 96> label{};
        const int written = observation.kind == triggers::Kind::volume
                                ? std::snprintf(label.data(),
                                                label.size(),
                                                "Havok trigger volume%s",
                                                observation.active ? " (occupied)" : "")
                                : std::snprintf(label.data(),
                                                label.size(),
                                                "Trigger event 0x%04X%s",
                                                observation.objectHandle,
                                                observation.active ? " (active)" : "");
        node.name = written > 0 && static_cast<std::size_t>(written) < label.size()
                        ? std::string(label.data(), static_cast<std::size_t>(written))
                        : std::string("Trigger event");
        node.searchText = "runtime native trigger event component active enabled selector source";
        node.kind = NodeKind::trigger;
        node.status = Status::known;
        node.producer = Producer::trigger;
        node.provenance = Provenance::runtime;
        node.source = source;
        node.observationId = trigger_identity(observation);
        node.nativeKey = *node.observationId;
        node.actions = Action::copyId;
        if (observation.kind == triggers::Kind::event) {
            node.triggerSelector = observation.selector;
            node.triggerSourceHash = observation.sourceHash;
        } else {
            node.triggerOverlapCount = observation.overlapCount;
            apply_runtime_position(node, observation.position, observation.positionPresent);
        }
        node.triggerEnabled = observation.enabled;
        node.triggerActive = observation.active;
        for (std::size_t objectIndex = 0;
             observation.kind == triggers::Kind::event && objectIndex < objectsSnapshot.objectCount;
             ++objectIndex) {
            const objects::Observation& object = objectsSnapshot.objects[objectIndex];
            if (static_cast<std::uint16_t>(object.handle) != observation.objectHandle) {
                continue;
            }
            node.runtimeEntity = object.handle;
            apply_runtime_position(node, object.position, object.positionPresent);
            break;
        }
        const NodeId id = graph.add(std::move(node), result.triggerGroup);
        if (!id) {
            break;
        }
        result.triggers.push_back(id);
    }
    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Trigger rows include copied c_trigger_event_component state and live hkpTriggerVolume "
         "positions. Native volume shape bounds are not recovered."});
}

inline void append_physics(Graph& graph,
                           std::vector<Diagnostic>& diagnostics,
                           const PhysicsSnapshot& snapshot,
                           const Source& source,
                           NodeId parent,
                           AppendResult& result) {
    if (!snapshot.present) {
        return;
    }

    Node group;
    std::array<char, 112> groupLabel{};
    const int groupWritten = snapshot.truncated
                                 ? std::snprintf(groupLabel.data(),
                                                 groupLabel.size(),
                                                 "Observed Havok body slots (%u/at least %u)",
                                                 static_cast<unsigned>(snapshot.bodyCount),
                                                 snapshot.declaredSlots)
                                 : std::snprintf(groupLabel.data(),
                                                 groupLabel.size(),
                                                 "Observed Havok body slots (%u/%u)",
                                                 static_cast<unsigned>(snapshot.bodyCount),
                                                 snapshot.declaredSlots);
    group.name = groupWritten > 0 && static_cast<std::size_t>(groupWritten) < groupLabel.size()
                     ? std::string(groupLabel.data(), static_cast<std::size_t>(groupWritten))
                     : std::string("Observed Havok body slots");
    group.searchText = "runtime havok physics bounded copied snapshot body slots";
    group.kind = NodeKind::physics;
    group.status = snapshot.truncated ? Status::deferred : Status::known;
    group.producer = Producer::physics;
    group.provenance = Provenance::runtime;
    group.source = source;
    group.actions = Action::copyId;
    result.physicsGroup = graph.add(std::move(group), parent);
    if (!result.physicsGroup) {
        diagnostics.push_back(
            {Diagnostic::Severity::error,
             "The inspection graph could not create the Havok observation group."});
        return;
    }

    result.physicsBodies.reserve(snapshot.bodyCount);
    for (std::size_t index = 0; index < snapshot.bodyCount; ++index) {
        const noclip::PhysicsBodyObservation& observation = snapshot.bodies[index];
        Node node;
        node.name = physics_label(observation.slot);
        node.searchText =
            "runtime havok physics copied body snapshot slot unknown identity position velocity";
        node.kind = NodeKind::physics;
        node.status = Status::unknownSemantic;
        node.producer = Producer::physics;
        node.provenance = Provenance::runtime;
        node.source = source;
        node.observationId = observation.slot;
        node.nativeKey = observation.slot;
        node.transform = Transform{observation.position};
        node.transformRuntime = true;
        node.linearVelocity = observation.velocity;
        node.actions =
            Action::focus | Action::hide | Action::isolate | Action::copyId | Action::copyPosition;
        const NodeId id = graph.add(std::move(node), result.physicsGroup);
        if (!id) {
            diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph reached its node-id capacity while adding Havok slots."});
            break;
        }
        result.physicsBodies.push_back(id);
    }

    diagnostics.push_back(
        {Diagnostic::Severity::information,
         "Physics rows are bounded copied Havok array slots. They are not durable body, object, "
         "controller, or entity identities."});
    if (snapshot.truncated) {
        diagnostics.push_back(
            {Diagnostic::Severity::warning,
             "The Havok observation exceeded a validation or copy bound and is partial."});
    }
}

[[nodiscard]] inline AppendResult append(Graph& graph,
                                         std::vector<Diagnostic>& diagnostics,
                                         const ObjectSnapshot& objectSnapshot,
                                         const TriggerSnapshot& triggerSnapshot,
                                         const AudioSnapshot& audioSnapshot,
                                         const PhysicsSnapshot& physicsSnapshot,
                                         const Source& source,
                                         NodeId parent) {
    AppendResult result{};
    append_objects(graph, diagnostics, objectSnapshot, source, parent, result);
    append_triggers(graph, diagnostics, triggerSnapshot, objectSnapshot, source, parent, result);
    append_audio(graph, diagnostics, audioSnapshot, source, parent, result);
    append_physics(graph, diagnostics, physicsSnapshot, source, parent, result);
    return result;
}

inline void update_triggers(Graph& graph,
                            std::span<const NodeId> nodeIds,
                            const TriggerSnapshot& snapshot,
                            const ObjectSnapshot& objectsSnapshot) noexcept {
    if (!snapshot.present) {
        return;
    }
    const std::span observations{snapshot.triggers.data(), snapshot.triggerCount};
    for (const NodeId nodeId : nodeIds) {
        Node* node = graph.node(nodeId);
        if (node == nullptr || !node->observationId.has_value()) {
            continue;
        }
        const auto observationIterator =
            std::ranges::find(observations, *node->observationId, trigger_identity);
        if (observationIterator == observations.end()) {
            continue;
        }
        const triggers::Observation& observation = *observationIterator;
        node->triggerEnabled = observation.enabled;
        node->triggerActive = observation.active;
        node->triggerOverlapCount = observation.kind == triggers::Kind::volume
                                        ? std::optional<std::uint32_t>{observation.overlapCount}
                                        : std::nullopt;
        node->name =
            observation.kind == triggers::Kind::volume
                ? std::string("Havok trigger volume") + (observation.active ? " (occupied)" : "")
                : std::string("Trigger event 0x") + [&observation] {
                      std::array<char, 32> text{};
                      (void)std::snprintf(text.data(),
                                          text.size(),
                                          "%04X%s",
                                          observation.objectHandle,
                                          observation.active ? " (active)" : "");
                      return std::string(text.data());
                  }();
        if (observation.kind == triggers::Kind::volume) {
            apply_runtime_position(*node, observation.position, observation.positionPresent);
            continue;
        }
        node->runtimeEntity.reset();
        node->transform.reset();
        node->transformRuntime = false;
        node->actions = Action::copyId;
        for (std::size_t objectIndex = 0; objectIndex < objectsSnapshot.objectCount;
             ++objectIndex) {
            const objects::Observation& object = objectsSnapshot.objects[objectIndex];
            if (static_cast<std::uint16_t>(object.handle) != observation.objectHandle) {
                continue;
            }
            node->runtimeEntity = object.handle;
            apply_runtime_position(*node, object.position, object.positionPresent);
            break;
        }
    }
}

inline void update_objects(Graph& graph,
                           std::span<const NodeId> nodeIds,
                           const ObjectSnapshot& snapshot) noexcept {
    if (!snapshot.present) {
        return;
    }
    const std::span observations{snapshot.objects.data(), snapshot.objectCount};
    for (const NodeId nodeId : nodeIds) {
        Node* node = graph.node(nodeId);
        if (node == nullptr || !node->runtimeEntity.has_value()) {
            continue;
        }
        const auto observationIterator = std::ranges::find(
            observations, *node->runtimeEntity, &objects::Observation::handle);
        if (observationIterator == observations.end()) {
            continue;
        }
        const objects::Observation& observation = *observationIterator;
        apply_runtime_position(*node, observation.position, observation.positionPresent);
    }
}

inline void update_audio(Graph& graph, NodeId nodeId, const AudioSnapshot& snapshot) noexcept {
    Node* node = graph.node(nodeId);
    if (node == nullptr || !snapshot.present || node->observationId != 0) {
        return;
    }
    apply_runtime_position(*node, snapshot.position, snapshot.present);
}

inline void update_physics(Graph& graph,
                           std::span<const NodeId> nodeIds,
                           const PhysicsSnapshot& snapshot) noexcept {
    if (!snapshot.present) {
        return;
    }
    const std::span observations{snapshot.bodies.data(), snapshot.bodyCount};
    for (const NodeId nodeId : nodeIds) {
        Node* node = graph.node(nodeId);
        if (node == nullptr || !node->observationId.has_value()) {
            continue;
        }
        const auto observationIterator = std::ranges::find(
            observations, *node->observationId, &noclip::PhysicsBodyObservation::slot);
        if (observationIterator == observations.end()) {
            continue;
        }
        const noclip::PhysicsBodyObservation& observation = *observationIterator;
        apply_runtime_position(*node, observation.position, true);
        node->linearVelocity = observation.velocity;
    }
}

} // namespace sunrise::client::inspection::providers::runtime_observations
