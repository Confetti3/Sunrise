#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::inspection::activity_logic_catalog {

inline constexpr std::uint32_t kSchemaVersion = 4;
inline constexpr std::uint32_t kCollectorVersion = 4;
inline constexpr std::size_t kDigestSize = 32;
inline constexpr std::size_t kHeaderSize = 240;
inline constexpr std::uint64_t kNonSpatialWorldId = (std::numeric_limits<std::uint64_t>::max)();
using Digest = std::array<std::uint8_t, kDigestSize>;

[[nodiscard]] constexpr bool is_spatial_world_id(std::uint64_t worldId) noexcept {
    return worldId != 0 && worldId != kNonSpatialWorldId;
}

enum class Role : std::uint8_t {
    actionSequence = 0,
    actionTarget = 1,
    competitiveRule = 2,
    conditionMonitor = 3,
    device = 4,
    object = 5,
    objective = 6,
    spatialRule = 7,
    spawnDefinition = 8,
    squadDefinition = 9,
    triggerSource = 10,
    unknown = 255,
};

enum class Confidence : std::uint8_t {
    unknown = 0,
    probable = 1,
    strong = 2,
};

struct Placement final {
    std::uint64_t worldId{};
    std::uint32_t mapTableTag{};
    std::uint32_t placedEntityTag{};
    std::array<float, 3> position{};
    std::array<float, 4> rotation{};
};

struct Entity final {
    std::uint32_t definitionTag{};
    std::uint32_t classPrimary{};
    std::uint32_t classSecondary{};
    Role role{Role::unknown};
    Confidence confidence{Confidence::unknown};
    std::string name;
    std::string label;
    std::string localizedText;
    std::vector<Placement> placements;
};

struct Activity final {
    std::uint32_t scenarioTag{};
    std::string name;
    std::string destination;
    std::vector<std::uint32_t> entityIndices;
};

struct Edge final {
    std::uint32_t sourceEntityIndex{};
    std::uint32_t targetEntityIndex{};
    std::uint32_t nameHash{};
    std::uint32_t occurrenceCount{};
};

struct StateVarTrigger final {
    std::int32_t lower{};
    std::int32_t upper{};
    std::uint32_t referenceTag{};
    std::uint32_t behaviorRootTag{};
};

struct StateVar final {
    std::uint32_t configTag{};
    std::uint32_t nameHash{};
    std::int32_t initial{};
    std::int32_t lowerClamp{-1};
    std::int32_t upperClamp{-1};
    std::int32_t lowerThreshold{-1};
    std::int32_t upperThreshold{-1};
    bool projectionEnabled{};
    std::uint32_t projectionBytecodeCount{};
    std::uint32_t projectionConstantCount{};
    std::string name;
    bool nameProved{};
    std::vector<StateVarTrigger> triggers;
};

struct StateVarBinding final {
    std::uint32_t ownerTag{};
    std::uint32_t configTag{};
    std::uint32_t definitionEntityIndex{};
};

struct LogicRoot final {
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::string name;
};

enum class LogicReferenceDirection : std::uint8_t {
    read,
    write,
};

struct LogicReference final {
    inline static constexpr std::uint32_t kUnjoinedStateVar = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t rootIndex{};
    std::uint32_t stateVarIndex{kUnjoinedStateVar};
    std::uint32_t nameHash{};
    std::uint32_t occurrenceCount{};
    std::int32_t selector{-1};
    LogicReferenceDirection direction{LogicReferenceDirection::read};
};

struct Provenance final {
    Digest contentFingerprint{};
    std::uint32_t collectorVersion{};
    std::uint32_t contentBuild{};
};

struct Catalog final {
    Provenance provenance{};
    std::vector<Activity> activities;
    std::vector<Entity> entities;
    std::vector<Edge> edges;
    std::vector<StateVar> stateVars;
    std::vector<StateVarBinding> stateVarBindings;
    std::vector<LogicRoot> logicRoots;
    std::vector<LogicReference> logicReferences;
    // Adjacency index built once during load: edge indices grouped by endpoint.
    // edgeBySourceOffsets has size entities.size()+1; edgeBySource is sorted by source entity.
    std::vector<std::uint32_t> edgeBySourceOffsets;
    std::vector<std::uint32_t> edgeBySource;
    std::vector<std::uint32_t> edgeByTargetOffsets;
    std::vector<std::uint32_t> edgeByTarget;
};

enum class LoadState : std::uint8_t {
    missing,
    malformed,
    ready,
};

struct LoadResult final {
    LoadState state{LoadState::missing};
    std::string diagnostic;
};

[[nodiscard]] bool validate(const Catalog& catalog, std::string& error);
[[nodiscard]] bool load(std::span<const std::byte> bytes, Catalog& catalog, std::string& error);
[[nodiscard]] LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept;
[[nodiscard]] const Activity* find_activity(const Catalog& catalog,
                                            std::uint32_t scenarioTag) noexcept;
[[nodiscard]] const char* role_name(Role role) noexcept;
[[nodiscard]] const char* confidence_name(Confidence confidence) noexcept;

/** Edge indices whose source is the entity at @p entityIndex. */
[[nodiscard]] std::span<const std::uint32_t> outgoing_edges(const Catalog& catalog,
                                                            std::uint32_t entityIndex) noexcept;
/** Edge indices whose target is the entity at @p entityIndex. */
[[nodiscard]] std::span<const std::uint32_t> incoming_edges(const Catalog& catalog,
                                                            std::uint32_t entityIndex) noexcept;

} // namespace sunrise::client::inspection::activity_logic_catalog
