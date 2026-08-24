#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::graph_admission {

struct Limits final {
    std::size_t siblingContext{12};
    std::size_t childLimit{48};
    std::size_t grandchildLimit{64};
    /** The all-filtered-nodes scope admits at most this many graph nodes. */
    std::size_t hierarchyLimit{512};
};

struct Result final {
    inspection::NodeId root{};
    std::size_t omitted{};
    bool truncated{};
};

[[nodiscard]] inline bool eligible(inspection::NodeId id,
                                   const std::unordered_set<std::uint64_t>& admitted) noexcept {
    return id && admitted.contains(id.value);
}

inline void admit(inspection::NodeId id,
                  const std::unordered_set<std::uint64_t>& eligibleNodes,
                  std::unordered_set<std::uint64_t>& output) {
    if (eligible(id, eligibleNodes)) {
        output.insert(id.value);
    }
}

[[nodiscard]] inline Result
selected_neighborhood(const inspection::Graph& graph,
                      inspection::NodeId selectedId,
                      const std::unordered_set<std::uint64_t>& eligibleNodes,
                      const std::unordered_set<std::uint64_t>& collapsed,
                      std::unordered_set<std::uint64_t>& output,
                      Limits limits = {}) {
    output.clear();
    const inspection::Node* selected = graph.node(selectedId);
    if (selected == nullptr || !eligible(selectedId, eligibleNodes)) {
        return {};
    }

    Result result;
    result.root = eligible(selected->parent, eligibleNodes) ? selected->parent : selectedId;
    admit(result.root, eligibleNodes, output);
    admit(selectedId, eligibleNodes, output);

    const inspection::Node* root = graph.node(result.root);
    if (root != nullptr && !collapsed.contains(result.root.value)) {
        std::vector<inspection::NodeId> siblings;
        siblings.reserve(root->children.size());
        for (const inspection::NodeId child : root->children) {
            if (eligible(child, eligibleNodes)) {
                siblings.push_back(child);
            }
        }
        const auto selectedIterator = std::ranges::find(siblings, selectedId);
        if (selectedIterator != siblings.end()) {
            const std::size_t selectedIndex =
                static_cast<std::size_t>(selectedIterator - siblings.begin());
            const std::size_t half = limits.siblingContext / 2U;
            const std::size_t first = selectedIndex > half ? selectedIndex - half : 0U;
            const std::size_t last =
                (std::min)(siblings.size(), first + limits.siblingContext + 1U);
            for (std::size_t index = first; index < last; ++index) {
                admit(siblings[index], eligibleNodes, output);
            }
        } else {
            for (std::size_t index = 0; index < (std::min)(siblings.size(), limits.siblingContext);
                 ++index) {
                admit(siblings[index], eligibleNodes, output);
            }
        }
    }

    if (!collapsed.contains(selectedId.value)) {
        std::size_t admittedChildren = 0;
        std::size_t admittedGrandchildren = 0;
        for (const inspection::NodeId childId : selected->children) {
            if (!eligible(childId, eligibleNodes)) {
                continue;
            }
            if (admittedChildren >= limits.childLimit) {
                continue;
            }
            admit(childId, eligibleNodes, output);
            ++admittedChildren;
            const inspection::Node* child = graph.node(childId);
            if (child == nullptr || collapsed.contains(childId.value)) {
                continue;
            }
            for (const inspection::NodeId grandchild : child->children) {
                if (!eligible(grandchild, eligibleNodes)
                    || admittedGrandchildren >= limits.grandchildLimit) {
                    continue;
                }
                admit(grandchild, eligibleNodes, output);
                ++admittedGrandchildren;
            }
        }
    }

    result.omitted =
        eligibleNodes.size() > output.size() ? eligibleNodes.size() - output.size() : 0U;
    result.truncated = result.omitted != 0;
    return result;
}

[[nodiscard]] inline Result
filtered_hierarchy(const inspection::Graph& graph,
                   inspection::NodeId rootId,
                   const std::unordered_set<std::uint64_t>& eligibleNodes,
                   const std::unordered_set<std::uint64_t>& collapsed,
                   std::unordered_set<std::uint64_t>& output,
                   Limits limits = {}) {
    output.clear();
    Result result;
    result.root = rootId;
    if (!eligible(rootId, eligibleNodes)) {
        return result;
    }

    std::vector<inspection::NodeId> pending{rootId};
    while (!pending.empty() && output.size() < limits.hierarchyLimit) {
        const inspection::NodeId id = pending.back();
        pending.pop_back();
        if (!eligible(id, eligibleNodes) || !output.insert(id.value).second) {
            continue;
        }
        const inspection::Node* node = graph.node(id);
        if (node == nullptr || collapsed.contains(id.value)) {
            continue;
        }
        for (auto child = node->children.rbegin(); child != node->children.rend(); ++child) {
            if (eligible(*child, eligibleNodes)) {
                pending.push_back(*child);
            }
        }
    }

    result.omitted =
        eligibleNodes.size() > output.size() ? eligibleNodes.size() - output.size() : 0U;
    result.truncated = result.omitted != 0 || !pending.empty();
    return result;
}

} // namespace sunrise::client::ui::world_inspector::graph_admission
