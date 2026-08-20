#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../world_inspection_model.h"
#include "live_player_inspection.h"
#include "placed_object_inspection.h"
#include "runtime_observation_inspection.h"
#include "activity_graph_inspection.h"

namespace sunrise::client::inspection::providers {

struct WorldSnapshot final {
    Graph graph;
    std::vector<Diagnostic> diagnostics;
    NodeId localPlayerNode{};
    NodeId runtimeObjectGroupNode{};
    std::vector<NodeId> runtimeObjectNodes;
    NodeId triggerGroupNode{};
    std::vector<NodeId> triggerNodes;
    NodeId audioListenerNode{};
    NodeId physicsGroupNode{};
    std::vector<NodeId> physicsBodyNodes;
    NodeId placedObjectGroupNode{};
    NodeId activityCatalogNode{};
    std::uint32_t activityCatalogBuild{};
    std::string activityCatalogVersion;
    std::string activityCatalogDiagnostic;
    NodeId spawnSetNode{};
    std::string packageName;
    std::string mapStem;
    std::size_t placedObjectCount{};
    std::size_t placedObjectSlotCount{};
    std::uint32_t physicsDeclaredSlots{};
    std::uint16_t physicsBodyCount{};
    std::uint64_t activitySession{};
    std::uint64_t activityRevision{};
    std::uint32_t scenarioTag{};
    std::uint32_t spawnSetHash{};
    std::uint32_t localPlayerHandle{};
    std::uint32_t runtimeObjectDeclaredCount{};
    std::uint16_t runtimeObjectCount{};
    std::uint16_t triggerCount{};
    std::int32_t activityIndex{-1};
    std::int32_t region{-1};
    std::optional<std::uint16_t> bubble;
    std::optional<std::uint16_t> mapBubble;
    bool sessionPresent{};
    bool scenarioCatalogReady{};
    bool scenarioPresent{};
    bool scenarioTruncated{};
    bool localPlayerPresent{};
    bool localPlayerPositionPresent{};
    bool runtimeObjectsPresent{};
    bool runtimeObjectsTruncated{};
    bool triggersPresent{};
    bool triggersTruncated{};
    bool placedObjectSlotsTruncated{};
    bool audioListenerPresent{};
    bool physicsPresent{};
    bool physicsTruncated{};
    bool spawnCatalogReady{};
    bool stale{};
    bool activityCatalogPresent{};
    bool activityCatalogBuildMatch{};
    /** Activity-scoped identity epoch for runtime producer handles. */
    std::uint32_t producerEpoch{1};
};

class SpawnInspectionProvider final {
public:
    /** Refreshes the graph only when current-world identity or readiness changes. */
    [[nodiscard]] bool refresh();

    [[nodiscard]] const WorldSnapshot& snapshot() const noexcept;
    void reset() noexcept;

private:
    struct Key final {
        std::string packageName;
        std::string mapStem;
        std::vector<placed_objects::Snapshot> placedObjects;
        std::array<std::uint64_t, hooks::noclip::kPhysicsObservationCapacity> physicsSlots{};
        std::array<std::uint32_t, ::sunrise::client::viewer::objects::kObservationCapacity>
            runtimeObjectHandles{};
        std::array<std::uint8_t, ::sunrise::client::viewer::objects::kObservationCapacity>
            runtimeObjectTypes{};
        std::array<std::uint64_t, ::sunrise::client::viewer::triggers::kObservationCapacity>
            triggerIdentities{};
        std::uint64_t activitySession{};
        std::uint64_t activityRevision{};
        std::uint32_t scenarioTag{};
        std::uint32_t spawnSetHash{};
        std::uint32_t localPlayerHandle{};
        std::uint32_t physicsDeclaredSlots{};
        std::uint32_t runtimeObjectDeclaredCount{};
        std::uint16_t physicsBodyCount{};
        std::uint16_t runtimeObjectCount{};
        std::uint16_t triggerCount{};
        std::int32_t activityIndex{-1};
        std::int32_t region{-1};
        std::int32_t bubble{-1};
        std::int32_t mapBubble{-1};
        bool sessionPresent{};
        bool scenarioReady{};
        bool scenarioPresent{};
        bool scenarioTruncated{};
        bool localPlayerPresent{};
        bool localPlayerPositionPresent{};
        bool runtimeObjectsPresent{};
        bool runtimeObjectsTruncated{};
        bool triggersPresent{};
        bool triggersTruncated{};
        bool audioListenerPresent{};
        bool physicsPresent{};
        bool physicsTruncated{};
        bool spawnCatalogReady{};
        bool stale{};

        [[nodiscard]] friend bool operator==(const Key&, const Key&) noexcept = default;
    };

    void rebuild(const Key& key,
                 const live_player::Snapshot& livePlayer,
                 const runtime_observations::ObjectSnapshot& objects,
                 const runtime_observations::TriggerSnapshot& triggers,
                 const runtime_observations::AudioSnapshot& audio,
                 const runtime_observations::PhysicsSnapshot& physics);

    WorldSnapshot snapshot_{};
    Key key_{};
    std::uint32_t generation_{};
    std::uint32_t producerEpoch_{};
    bool keyPresent_{};
};

} // namespace sunrise::client::inspection::providers
