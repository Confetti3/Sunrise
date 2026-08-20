#pragma once

#include <cstdint>
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
    std::vector<LayoutNode> layout;
    bool fitRequested{true};
};

struct Result final {
    inspection::NodeId hovered{};
    inspection::NodeId selected{};
    inspection::NodeId focused{};
    inspection::NodeId context{};
    bool clearSelection{};
};
[[nodiscard]] Result draw(const inspection::Graph& graph,
                          inspection::NodeId root,
                          inspection::NodeId selected,
                          const std::unordered_set<std::uint64_t>& admitted,
                          std::uint64_t admissionRevision,
                          State& state) noexcept;

} // namespace sunrise::client::ui::world_inspector::graph
