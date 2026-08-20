#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::inspection {

struct NodeId final {
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return value != 0;
    }

    [[nodiscard]] friend bool operator==(NodeId, NodeId) noexcept = default;
};

enum class NodeKind : std::uint8_t {
    world,
    source,
    activity,
    activityGraph,
    activityGraphNode,
    activityReference,
    destination,
    spawnGroup,
    spawnSet,
    spawnPoint,
    geometry,
    terrain,
    runtimeEntity,
    placedObject,
    componentSlot,
    light,
    trigger,
    audio,
    physics,
    navigation,
    unresolved,
};

enum class Status : std::uint8_t {
    known,
    unknownSemantic,
    deferred,
    failed,
};

/** Owns the copied values represented by one inspection node. */
enum class Producer : std::uint8_t {
    graph,
    catalog,
    activityCatalog,
    localPlayer,
    objectSystem,
    trigger,
    audioListener,
    physics,
    audioEmitter,
    navigation,
    light,
    terrain,
};

enum class Provenance : std::uint8_t {
    derived,
    catalog,
    runtime,
};

enum class Action : std::uint32_t {
    none = 0,
    focus = 1U << 0U,
    hide = 1U << 1U,
    isolate = 1U << 2U,
    copyId = 1U << 3U,
    copyPosition = 1U << 4U,
    copyTag = 1U << 5U,
    openTag = 1U << 6U,
};

[[nodiscard]] constexpr Action operator|(Action left, Action right) noexcept {
    return static_cast<Action>(static_cast<std::uint32_t>(left)
                               | static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool supports(Action actions, Action candidate) noexcept {
    return (static_cast<std::uint32_t>(actions) & static_cast<std::uint32_t>(candidate)) != 0;
}

struct Transform final {
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
    bool hasRotation{};
    bool hasScale{};
    [[nodiscard]] friend bool operator==(const Transform&, const Transform&) noexcept = default;
};

struct Bounds final {
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    [[nodiscard]] friend bool operator==(const Bounds&, const Bounds&) noexcept = default;
};

[[nodiscard]] bool bounds_valid(const Bounds& bounds) noexcept;
[[nodiscard]] std::array<float, 3> bounds_center(const Bounds& bounds) noexcept;
[[nodiscard]] std::array<float, 3> bounds_extents(const Bounds& bounds) noexcept;
[[nodiscard]] std::array<std::array<float, 3>, 8> bounds_corners(const Bounds& bounds) noexcept;

[[nodiscard]] constexpr Action positional_actions() noexcept {
    return Action::focus | Action::hide | Action::isolate | Action::copyPosition;
}

[[nodiscard]] constexpr Action identity_actions() noexcept {
    return Action::copyId;
}

[[nodiscard]] constexpr Action spatial_actions(Action additional = Action::none) noexcept {
    return positional_actions() | identity_actions() | additional;
}


struct Source final {
    std::string packageName;
    std::string mapStem;
    std::optional<std::uint32_t> scenarioTag;
    std::optional<std::uint32_t> spawnSetHash;
    std::optional<std::uint64_t> activitySession;
    std::optional<std::int32_t> activityIndex;
    std::optional<std::uint16_t> bubble;
    [[nodiscard]] friend bool operator==(const Source&, const Source&) noexcept = default;
};

struct ActivityMetadata final {
    std::uint32_t activityHash{};
    std::uint32_t graphHash{};
    std::uint32_t nodeHash{};
    std::uint32_t stateHash{};
    std::uint32_t styleHash{};
    std::array<float, 2> authoredPosition{};
    std::uint32_t releaseCount{};
    std::uint32_t referenceCount{};
    std::uint32_t catalogBuild{};
    std::string catalogVersion;
    std::vector<std::uint32_t> linkedGraphHashes;
    bool buildMatch{};
    bool browseOnly{true};

};
struct Node final {
    NodeId id{};
    NodeId parent{};
    std::vector<NodeId> children;
    std::string name;
    std::string searchText;
    NodeKind kind{NodeKind::unresolved};
    Status status{Status::unknownSemantic};
    Producer producer{Producer::graph};
    Provenance provenance{Provenance::derived};
    /** Producer-owned scalar identity. Zero means the graph derives a stable structural key. */
    std::uint64_t nativeKey{};
    Source source{};
    std::optional<ActivityMetadata> activityMetadata;
    std::optional<std::uint64_t> runtimeEntity;
    std::optional<std::uint8_t> objectSystemType;
    /** Runtime producer-local observation slot; not necessarily a durable engine identity. */
    std::optional<std::uint64_t> observationId;
    std::optional<std::int32_t> triggerSelector;
    std::optional<std::uint32_t> triggerSourceHash;
    std::optional<std::uint32_t> triggerOverlapCount;
    std::optional<bool> triggerEnabled;
    std::optional<bool> triggerActive;
    std::optional<std::uint64_t> worldId;
    std::optional<std::uint32_t> tag;
    std::optional<std::uint32_t> classHash;
    std::optional<std::uint32_t> nameHash;
    std::optional<Transform> transform;
    /** True when transform came from a copied live observation instead of package data. */
    bool transformRuntime{};
    std::optional<std::array<float, 3>> linearVelocity;
    std::optional<Bounds> bounds;
    std::optional<Provenance> boundsProvenance;
    Action actions{Action::none};
};
[[nodiscard]] constexpr bool has_spatial_data(const Node& node) noexcept {
    return node.transform.has_value() || node.bounds.has_value();
}

struct Diagnostic final {
    enum class Severity : std::uint8_t {
        information,
        warning,
        error,
    };

    Severity severity{Severity::information};
    std::string message;
};

class Graph final {
public:
    void reset(std::uint32_t generation);
    [[nodiscard]] NodeId add(Node node, NodeId parent = {});

    [[nodiscard]] const Node* node(NodeId id) const noexcept;
    [[nodiscard]] Node* node(NodeId id) noexcept;
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept;
    [[nodiscard]] NodeId root() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept;
    [[nodiscard]] std::string breadcrumb(NodeId id) const;

private:
    [[nodiscard]] NodeId id_for_index(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> index_for_id(NodeId id) const noexcept;

    std::vector<Node> nodes_;
    NodeId root_{};
    std::uint32_t generation_{1};
};

struct QueryTerm final {
    enum class Field : std::uint8_t {
        any,
        type,
        world,
        tag,
        classHash,
        status,
    };

    Field field{Field::any};
    std::string value;
};

struct Query final {
    std::vector<QueryTerm> terms;
};

[[nodiscard]] Query parse_query(std::string_view text);
[[nodiscard]] bool matches(const Node& node, const Query& query) noexcept;
[[nodiscard]] const char* kind_name(NodeKind kind) noexcept;
[[nodiscard]] const char* status_name(Status status) noexcept;
[[nodiscard]] const char* producer_name(Producer producer) noexcept;
[[nodiscard]] const char* provenance_name(Provenance provenance) noexcept;

class Selection final {
public:
    void reconcile(const Graph& graph) noexcept;
    void select(NodeId id) noexcept;
    void hover(NodeId id) noexcept;
    void clear() noexcept;
    void clear_hover() noexcept;

    [[nodiscard]] NodeId selected() const noexcept;
    [[nodiscard]] NodeId hovered() const noexcept;

private:
    NodeId selected_{};
    NodeId hovered_{};
};

} // namespace sunrise::client::inspection
