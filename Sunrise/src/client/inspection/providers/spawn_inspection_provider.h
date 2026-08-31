#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../inspection_document.h"
#include "../world_inspection_model.h"
#include "activity_graph_inspection.h"
#include "activity_logic_inspection.h"
#include "bubble_bounds_inspection.h"
#include "live_player_inspection.h"
#include "placed_object_inspection.h"
#include "runtime_observation_inspection.h"
#include "statics_footprint_inspection.h"

namespace sunrise::client::inspection::providers {

enum class RefreshKind : std::uint8_t {
    none,
    value,
    structural,
};

struct RefreshResult final {
    RefreshKind kind{RefreshKind::none};
    std::uint64_t valueRevision{};

    [[nodiscard]] constexpr operator bool() const noexcept {
        return kind != RefreshKind::none;
    }
    [[nodiscard]] constexpr bool rebuilt() const noexcept {
        return kind == RefreshKind::structural;
    }
    [[nodiscard]] constexpr bool changed() const noexcept {
        return kind != RefreshKind::none;
    }
};

class SpawnInspectionProvider final {
public:
    /** Refreshes copied world state and classifies no-op, value-only, and graph rebuild changes. */
    [[nodiscard]] RefreshResult refresh();

    /**
     * Selects continuous bounded runtime membership sampling. Intended for explicit research
     * capture because busy maps can rebuild the inspection graph frequently.
     */
    void set_live_runtime_membership(bool enabled) noexcept;

    /** Returns the current pointer-free inspection document. */
    [[nodiscard]] const InspectionDocument& snapshot() const noexcept;
    void reset() noexcept;

private:
    struct PublishedNodes final {
        NodeId localPlayer{};
        std::vector<NodeId> runtimeObjects;
        std::vector<NodeId> triggers;
        NodeId audioListener{};
        std::vector<NodeId> physicsBodies;
    };

    struct PlacedCacheKey final {
        std::string packageName;
        std::string mapStem;
        std::uint64_t activitySession{};
        std::uint64_t activityRevision{};
        std::uint32_t scenarioTag{};
        std::int32_t bubble{-1};

        [[nodiscard]] friend bool operator==(const PlacedCacheKey&,
                                             const PlacedCacheKey&) noexcept = default;
    };

    struct Key final {
        std::string packageName;
        std::string mapStem;
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
    void synchronize_document(RefreshKind kind,
                              const runtime_observations::ObjectSnapshot& objects,
                              const runtime_observations::TriggerSnapshot& triggers,
                              const runtime_observations::PhysicsSnapshot& physics) noexcept;

    InspectionDocument document_{};
    PublishedNodes publishedNodes_{};
    Key key_{};
    std::uint32_t generation_{};
    std::uint32_t producerEpoch_{};
    std::uint64_t valueRevision_{};
    bool liveRuntimeMembership_{};
    PlacedCacheKey placedCacheKey_{};
    std::vector<placed_objects::Snapshot> placedCache_;
    bool placedCachePresent_{};
    bool keyPresent_{};
};

} // namespace sunrise::client::inspection::providers
