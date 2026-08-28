#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::overview {

struct ActivityScope final {
    std::string label;
    std::uint64_t activitySession{};
    std::uint32_t scenarioTag{};
    bool available{};
    bool preview{};
};

enum class EdgeKind : std::uint8_t {
    ownership,
    reference,
    logic,
    authoredLink,
    runtimeAssociation,
    activityGraphLink,
    logicVariableRead,
    logicVariableWrite,
};

enum class Lane : std::uint8_t { context, behaviorRoot, variable, stateVarOwner };

struct Node final {
    inspection::NodeId source{};
    std::uint64_t identity{};
    Lane lane{Lane::context};
    bool hub{};
};

struct Edge final {
    std::size_t source{};
    std::size_t target{};
    EdgeKind kind{EdgeKind::ownership};
    std::uint32_t occurrenceCount{1};
    std::uint32_t nameHash{};
    std::int32_t selector{-1};
    [[nodiscard]] friend bool operator==(const Edge&, const Edge&) noexcept = default;
};

struct Model final {
    ActivityScope scope;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::size_t definitionCount{};
    std::size_t variableCount{};
    std::size_t variableAccessEdgeCount{};
    std::size_t unresolvedNameCount{};
    std::size_t compacted{};
    std::size_t omitted{};
    std::uint64_t revision{};
};

[[nodiscard]] bool belongs_to_scope(const inspection::Node& node,
                                    const ActivityScope& scope) noexcept;

[[nodiscard]] Model build(const inspection::Graph& graph,
                          const std::unordered_set<std::uint64_t>& eligible,
                          const ActivityScope& scope,
                          std::size_t maximumNodes,
                          inspection::NodeId selected = {});

/** Produces deterministic, finite force-directed positions aligned with Model::nodes. */
void layout(const Model& model, std::vector<std::array<float, 2>>& positions);

} // namespace sunrise::client::ui::world_inspector::overview
