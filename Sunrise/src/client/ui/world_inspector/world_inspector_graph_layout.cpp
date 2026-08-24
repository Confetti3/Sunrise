#include "world_inspector_graph_layout.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <vector>

#include "world_inspector_graph_geometry.h"

namespace sunrise::client::ui::world_inspector::graph_layout {
namespace {

constexpr float kCardWidth = graph_geometry::kCardWidth;
constexpr float kCardHeight = graph_geometry::kCardHeight;
constexpr float kHorizontalGap = 110.0F;
constexpr float kVerticalGap = 28.0F;

} // namespace

void compute(const inspection::Graph& graph,
             inspection::NodeId root,
             const std::unordered_set<std::uint64_t>& admitted,
             std::vector<LayoutNode>& output) {
    output.clear();
    if (!root || graph.node(root) == nullptr) {
        return;
    }
    // The graph is producer data and can contain duplicate links or cycles.  A
    // global visited set keeps layout deterministic and guarantees bounded work
    // even when a malformed producer repeats a node many times.
    std::unordered_set<std::uint64_t> visited;
    visited.insert(root.value);
    const float row = kCardHeight + kVerticalGap;
    const float column = kCardWidth + kHorizontalGap;
    std::size_t leaf = 0;
    struct Frame final {
        inspection::NodeId id{};
        std::uint8_t depth{};
        std::size_t nextChild{};
        float childY{};
        std::size_t childCount{};
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{root, 0, 0, 0.0F, 0});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        const inspection::Node* node = graph.node(frame.id);
        if (node == nullptr) {
            stack.pop_back();
            continue;
        }
        bool pushed = false;
        while (frame.nextChild < node->children.size()) {
            const inspection::NodeId child = node->children[frame.nextChild++];
            if (!child || !admitted.contains(child.value) || !graph.node(child)
                || !visited.insert(child.value).second) {
                continue;
            }
            const std::uint8_t childDepth =
                frame.depth == (std::numeric_limits<std::uint8_t>::max)()
                    ? frame.depth
                    : static_cast<std::uint8_t>(frame.depth + 1U);
            stack.push_back(Frame{child, childDepth, 0, 0.0F, 0});
            pushed = true;
            break;
        }
        if (pushed) {
            continue;
        }
        const float y = frame.childCount == 0 ? static_cast<float>(leaf++) * row
                                              : frame.childY / static_cast<float>(frame.childCount);
        output.push_back(
            LayoutNode{frame.id, {static_cast<float>(frame.depth) * column, y}, frame.depth});
        stack.pop_back();
        if (!stack.empty()) {
            stack.back().childY += y;
            ++stack.back().childCount;
        }
    }
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
