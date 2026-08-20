#include "world_inspector_graph_layout.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

namespace sunrise::client::ui::world_inspector::graph_layout {
namespace {

constexpr float kCardWidth = 190.0F;
constexpr float kCardHeight = 58.0F;
constexpr float kHorizontalGap = 78.0F;
constexpr float kVerticalGap = 26.0F;

} // namespace

void compute(const inspection::Graph& graph,
             inspection::NodeId root,
             const std::unordered_set<std::uint64_t>& admitted,
             std::vector<LayoutNode>& output) {
    output.clear();
    if (!root || graph.node(root) == nullptr) {
        return;
    }
    std::unordered_set<std::uint64_t> visiting;
    const float row = kCardHeight + kVerticalGap;
    const float column = kCardWidth + kHorizontalGap;
    std::size_t leaf = 0;
    const std::function<float(inspection::NodeId, std::uint8_t)> place =
        [&](inspection::NodeId id, std::uint8_t depth) -> float {
        if (!id || !visiting.insert(id.value).second) {
            return 0.0F;
        }
        const inspection::Node* node = graph.node(id);
        if (node == nullptr) {
            visiting.erase(id.value);
            return 0.0F;
        }
        float y = 0.0F;
        std::size_t childCount = 0;
        for (const inspection::NodeId child : node->children) {
            if (!admitted.contains(child.value)) {
                continue;
            }
            y += place(child,
                       depth == (std::numeric_limits<std::uint8_t>::max)()
                           ? depth
                           : static_cast<std::uint8_t>(depth + 1U));
            ++childCount;
        }
        if (childCount == 0) {
            y = static_cast<float>(leaf++) * row;
        } else {
            y /= static_cast<float>(childCount);
        }
        output.push_back(LayoutNode{id, {static_cast<float>(depth) * column, y}, depth});
        visiting.erase(id.value);
        return y;
    };
    (void)place(root, 0);
    std::ranges::stable_sort(output, [](const LayoutNode& left, const LayoutNode& right) {
        if (left.position[0] != right.position[0]) {
            return left.position[0] < right.position[0];
        }
        if (left.position[1] != right.position[1]) {
            return left.position[1] < right.position[1];
        }
        return left.id.value < right.id.value;
    });
}

} // namespace sunrise::client::ui::world_inspector::graph_layout
