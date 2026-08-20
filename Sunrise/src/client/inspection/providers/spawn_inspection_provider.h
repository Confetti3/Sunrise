#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../world_inspection_model.h"
#include "live_player_inspection.h"
#include "placed_object_inspection.h"

namespace sunrise::client::inspection::providers {

struct WorldSnapshot final {
    Graph graph;
    std::vector<Diagnostic> diagnostics;
    NodeId localPlayerNode{};
    NodeId placedObjectGroupNode{};
    NodeId spawnSetNode{};
    std::string packageName;
    std::string mapStem;
    std::size_t placedObjectCount{};
    std::size_t placedObjectSlotCount{};
    std::uint64_t activitySession{};
    std::uint64_t activityRevision{};
    std::uint32_t scenarioTag{};
    std::uint32_t spawnSetHash{};
    std::uint32_t localPlayerHandle{};
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
    bool placedObjectSlotsTruncated{};
    bool spawnCatalogReady{};
    bool stale{};
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
        std::uint64_t activitySession{};
        std::uint64_t activityRevision{};
        std::uint32_t scenarioTag{};
        std::uint32_t spawnSetHash{};
        std::uint32_t localPlayerHandle{};
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
        bool spawnCatalogReady{};
        bool stale{};

        [[nodiscard]] friend bool operator==(const Key&, const Key&) noexcept = default;
    };

    void rebuild(const Key& key, const live_player::Snapshot& livePlayer);

    WorldSnapshot snapshot_{};
    Key key_{};
    std::uint32_t generation_{};
    bool keyPresent_{};
};

} // namespace sunrise::client::inspection::providers
