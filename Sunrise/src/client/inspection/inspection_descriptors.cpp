#include "inspection_descriptors.h"

#include <array>

namespace sunrise::client::inspection {
namespace {

constexpr std::array kNodeKinds{
    NodeKindDescriptor{NodeKind::world, "world", "World", NodeCategory::context},
    NodeKindDescriptor{NodeKind::source, "source", "Source", NodeCategory::context},
    NodeKindDescriptor{NodeKind::activity, "activity", "Activity", NodeCategory::context},
    NodeKindDescriptor{
        NodeKind::activityGraph, "activity-graph", "Activity Graph", NodeCategory::authored},
    NodeKindDescriptor{NodeKind::activityGraphNode,
                       "activity-graph-node",
                       "Activity Graph Node",
                       NodeCategory::authored},
    NodeKindDescriptor{NodeKind::activityReference,
                       "activity-reference",
                       "Activity Reference",
                       NodeCategory::authored},
    NodeKindDescriptor{
        NodeKind::activityLogic, "activity-logic", "Activity Logic", NodeCategory::logic},
    NodeKindDescriptor{NodeKind::logicGroup, "logic-group", "Logic Group", NodeCategory::logic},
    NodeKindDescriptor{
        NodeKind::logicEntity, "logic-definition", "Logic Definition", NodeCategory::logic},
    NodeKindDescriptor{
        NodeKind::logicPlacement, "logic-placement", "Logic Placement", NodeCategory::logic},
    NodeKindDescriptor{NodeKind::destination, "destination", "Destination", NodeCategory::context},
    NodeKindDescriptor{NodeKind::spawnSet, "spawn-set", "Spawn Set", NodeCategory::spawn},
    NodeKindDescriptor{NodeKind::spawnPoint, "spawn-point", "Spawn Point", NodeCategory::spawn},
    NodeKindDescriptor{NodeKind::geometry, "geometry", "Geometry", NodeCategory::geometry},
    NodeKindDescriptor{
        NodeKind::runtimeEntity, "runtime-entity", "Runtime Entity", NodeCategory::entity},
    NodeKindDescriptor{
        NodeKind::placedObject, "placed-object", "Placed Object", NodeCategory::entity},
    NodeKindDescriptor{
        NodeKind::componentSlot, "component-slot", "Component Slot", NodeCategory::entity},
    NodeKindDescriptor{NodeKind::trigger, "trigger", "Trigger", NodeCategory::trigger},
    NodeKindDescriptor{NodeKind::audio, "audio", "Audio", NodeCategory::audio},
    NodeKindDescriptor{NodeKind::physics, "physics", "Physics", NodeCategory::physics},
    NodeKindDescriptor{NodeKind::unresolved, "unresolved", "Unresolved", NodeCategory::unresolved},
};

constexpr std::array kProducers{
    ProducerDescriptor{Producer::graph, "graph", "Graph", false},
    ProducerDescriptor{Producer::catalog, "catalog", "Catalog", true},
    ProducerDescriptor{Producer::activityCatalog, "activity-catalog", "Activity map catalog", true},
    ProducerDescriptor{
        Producer::activityLogicCatalog, "activity-logic-catalog", "Activity logic catalog", true},
    ProducerDescriptor{Producer::localPlayer, "local-player", "Local player", true},
    ProducerDescriptor{Producer::objectSystem, "object-system", "Object system", true},
    ProducerDescriptor{Producer::trigger, "trigger", "Trigger observations", true},
    ProducerDescriptor{Producer::audioListener, "audio-listener", "Audio listener", true},
    ProducerDescriptor{Producer::physics, "physics", "Physics observations", true},
    ProducerDescriptor{Producer::worldContext, "world-context", "World context", false},
    ProducerDescriptor{Producer::placedContent, "placed-content", "Placed content", true},
    ProducerDescriptor{Producer::spawnPoints, "spawn-points", "Spawn points", true},
    ProducerDescriptor{Producer::staticFootprints, "static-footprints", "Static footprints", true},
    ProducerDescriptor{Producer::bubbleBounds, "bubble-bounds", "Bubble bounds", true},
};

static_assert(kNodeKinds.size() == static_cast<std::size_t>(NodeKind::unresolved) + 1);
static_assert(kProducers.size() == static_cast<std::size_t>(Producer::bubbleBounds) + 1);

} // namespace

std::span<const NodeKindDescriptor> node_kind_descriptors() noexcept {
    return kNodeKinds;
}

std::span<const ProducerDescriptor> producer_descriptors() noexcept {
    return kProducers;
}

const NodeKindDescriptor& descriptor(NodeKind kind) noexcept {
    const std::size_t index = static_cast<std::size_t>(kind);
    return index < kNodeKinds.size() ? kNodeKinds[index] : kNodeKinds.back();
}

const ProducerDescriptor& descriptor(Producer producer) noexcept {
    const std::size_t index = static_cast<std::size_t>(producer);
    return index < kProducers.size() ? kProducers[index] : kProducers.front();
}

} // namespace sunrise::client::inspection
