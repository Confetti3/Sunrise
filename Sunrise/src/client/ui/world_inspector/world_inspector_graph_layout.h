#pragma once
#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::graph_layout {

struct LayoutNode final {
    inspection::NodeId id{};
    std::array<float, 2> position{};
    std::uint8_t depth{};
    [[nodiscard]] friend bool operator==(const LayoutNode&, const LayoutNode&) noexcept = default;
};
void compute(const inspection::Graph& graph,
             inspection::NodeId root,
             const std::unordered_set<std::uint64_t>& admitted,
             std::vector<LayoutNode>& output);

} // namespace sunrise::client::ui::world_inspector::graph_layout
