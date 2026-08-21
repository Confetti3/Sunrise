#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "world_inspector_graph_layout.h"
#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::graph {

using LayoutNode = graph_layout::LayoutNode;

struct State final {
    ImVec2 pan{};
    float zoom{1.0F};
    std::uint32_t cachedGeneration{};
    std::uint64_t cachedAdmissionRevision{};
    inspection::NodeId cachedRoot{};
    inspection::NodeId centerRequested{};
    std::vector<LayoutNode> layout;
    std::unordered_map<std::uint64_t, std::size_t> layoutIndex;
    bool fitRequested{true};
};

struct Result final {
    inspection::NodeId hovered{};
    inspection::NodeId selected{};
    inspection::NodeId focused{};
    inspection::NodeId context{};
    bool clearSelection{};
};

void reset(State& state) noexcept;

[[nodiscard]] Result draw(const inspection::Graph& graph,
                          inspection::NodeId root,
                          inspection::NodeId selected,
                          const std::unordered_set<std::uint64_t>& admitted,
                          std::uint64_t admissionRevision,
                          std::size_t omittedCount,
                          State& state) noexcept;

[[nodiscard]] Result draw_activity_logic_relationships(
    const inspection::Graph& graph,
    inspection::NodeId selected,
    State& state) noexcept;

} // namespace sunrise::client::ui::world_inspector::graph
