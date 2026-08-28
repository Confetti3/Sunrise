#pragma once

#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../inspection/world_inspection_model.h"
#include "world_inspector_graph_layout.h"
#include "world_inspector_overview.h"

namespace sunrise::client::ui::world_inspector::graph {

using LayoutNode = graph_layout::LayoutNode;

struct State final {
    ImVec2 pan{};
    float zoom{1.0F};
    std::uint32_t cachedGeneration{};
    std::uint64_t cachedLayoutRevision{};
    inspection::NodeId cachedRoot{};
    /** Identity hash of the cached root, stable across NodeId renumbering churn. */
    std::uint64_t cachedRootIdentity{};
    inspection::NodeId centerRequested{};
    std::vector<LayoutNode> layout;
    std::unordered_map<std::uint64_t, std::size_t> layoutIndex;
    bool fitRequested{true};
};

struct Result final {
    inspection::NodeId hovered{};
    inspection::NodeId selected{};
    inspection::NodeId context{};
    bool clearSelection{};
};

struct OverviewState final {
    ImVec2 pan{};
    float zoom{1.0F};
    std::uint64_t cachedInputRevision{};
    inspection::NodeId cachedSelected{};
    inspection::NodeId centerRequested{};
    overview::Model model;
    std::vector<std::array<float, 2>> positions;
    bool fitRequested{true};
};

void reset(State& state) noexcept;

void reset(OverviewState& state) noexcept;

[[nodiscard]] Result draw_overview(const inspection::Graph& graph,
                                   const std::unordered_set<std::uint64_t>& eligible,
                                   std::uint64_t admissionRevision,
                                   const overview::ActivityScope& scope,
                                   std::size_t maximumNodes,
                                   inspection::NodeId selected,
                                   OverviewState& state) noexcept;

[[nodiscard]] Result draw(const inspection::Graph& graph,
                          inspection::NodeId root,
                          inspection::NodeId selected,
                          const std::unordered_set<std::uint64_t>& admitted,
                          std::uint64_t admissionRevision,
                          std::size_t omittedCount,
                          State& state) noexcept;

[[nodiscard]] Result draw_activity_logic_relationships(const inspection::Graph& graph,
                                                       inspection::NodeId selected,
                                                       State& state) noexcept;

} // namespace sunrise::client::ui::world_inspector::graph
