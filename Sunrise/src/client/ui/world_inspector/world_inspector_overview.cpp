#include "world_inspector_overview.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace sunrise::client::ui::world_inspector::overview {
namespace {

constexpr std::uint64_t kHubIdentity = 0xA13E'7EED'BADC'0FFEULL;
constexpr std::size_t kDefinitionSiblingLimit = 2;
constexpr std::size_t kPlacementSiblingLimit = 1;
constexpr std::uint32_t kLogicRootClass = 0x8080941EU;
constexpr std::uint32_t kStateVarOwnerClass = 0x80809C0FU;

struct EdgeKey final {
    std::uint64_t source{};
    std::uint64_t target{};
    EdgeKind kind{EdgeKind::ownership};
    std::uint32_t occurrenceCount{};
    std::uint32_t nameHash{};
    std::int32_t selector{-1};
    [[nodiscard]] friend bool operator==(const EdgeKey&, const EdgeKey&) noexcept = default;
};

struct EdgeKeyHash final {
    [[nodiscard]] std::size_t operator()(const EdgeKey& value) const noexcept {
        std::uint64_t hash = value.source ^ (value.target + 0x9E3779B97F4A7C15ULL
                                              + (value.source << 6U)
                                              + (value.source >> 2U));
        hash ^= static_cast<std::uint64_t>(value.kind) << 56U;
        hash ^= static_cast<std::uint64_t>(value.occurrenceCount) << 24U;
        hash ^= value.nameHash;
        return static_cast<std::size_t>(hash);
    }
};

[[nodiscard]] std::uint64_t mix(std::uint64_t value, std::uint64_t lane) noexcept {
    value ^= lane + 0x9E3779B97F4A7C15ULL + (value << 6U) + (value >> 2U);
    return value;
}

[[nodiscard]] std::uint64_t node_identity(const inspection::Node& node) noexcept {
    const std::uint64_t value = static_cast<std::uint64_t>(inspection::NodeKeyHash{}(node.key));
    return value == 0 ? node.id.value : value;
}

[[nodiscard]] Lane node_lane(const inspection::Node& node) noexcept {
    if (node.kind == inspection::NodeKind::logicVariable) {
        return Lane::variable;
    }
    if (node.classHash.has_value() && *node.classHash == kLogicRootClass) {
        return Lane::behaviorRoot;
    }
    if (node.classHash.has_value() && *node.classHash == kStateVarOwnerClass) {
        return Lane::stateVarOwner;
    }
    return Lane::context;
}

[[nodiscard]] bool has_priority_relation(const inspection::Node& node) noexcept {
    return std::ranges::any_of(node.relations, [](const inspection::Relation& relation) {
        return relation.kind == inspection::RelationKind::authoredLink
               || relation.kind == inspection::RelationKind::logicVariableRead
               || relation.kind == inspection::RelationKind::logicVariableWrite;
    });
}

[[nodiscard]] unsigned node_priority(const inspection::Node& node) noexcept {
    if ((node.kind == inspection::NodeKind::activityGraph && node.activityMetadata.has_value())
        || node.kind == inspection::NodeKind::logicVariable || has_priority_relation(node)) {
        return 0;
    }
    switch (node.kind) {
    case inspection::NodeKind::world:
    case inspection::NodeKind::source:
    case inspection::NodeKind::activity:
    case inspection::NodeKind::activityLogic:
    case inspection::NodeKind::destination:
    case inspection::NodeKind::spawnSet:
        return 1;
    case inspection::NodeKind::logicGroup:
        return node.children.empty() ? 3U : 1U;
    default:
        return node.children.empty() ? 3U : 2U;
    }
}

[[nodiscard]] EdgeKind edge_kind(inspection::RelationKind kind) noexcept {
    switch (kind) {
    case inspection::RelationKind::reference:
        return EdgeKind::reference;
    case inspection::RelationKind::logic:
        return EdgeKind::logic;
    case inspection::RelationKind::authoredLink:
        return EdgeKind::authoredLink;
    case inspection::RelationKind::runtimeAssociation:
        return EdgeKind::runtimeAssociation;
    case inspection::RelationKind::logicVariableRead:
        return EdgeKind::logicVariableRead;
    case inspection::RelationKind::logicVariableWrite:
        return EdgeKind::logicVariableWrite;
    }
    return EdgeKind::reference;
}

[[nodiscard]] std::uint32_t relationship_name_hash(
    const inspection::Node& node,
    const inspection::Node& other,
    const inspection::Relation& relation) noexcept {
    if (!node.activityLogicMetadata.has_value() || !other.activityLogicMetadata.has_value()) {
        return relation.nameHash;
    }
    const std::uint32_t otherDefinition = other.activityLogicMetadata->definitionTag;
    const auto& relationships = node.activityLogicMetadata->relationships;
    const auto found = std::ranges::find_if(relationships, [&](const auto& candidate) {
        return candidate.definitionTag == otherDefinition
               && candidate.occurrenceCount == relation.occurrenceCount
               && candidate.outgoing == relation.outgoing;
    });
    return found == relationships.end() ? relation.nameHash : found->nameHash;
}

[[nodiscard]] std::uint64_t model_revision(const Model& model) noexcept {
    std::uint64_t value = 0xCBF29CE484222325ULL;
    value = mix(value, model.scope.activitySession);
    value = mix(value, model.scope.scenarioTag);
    value = mix(value, model.scope.preview ? 1U : 0U);
    value = mix(value, model.compacted);
    value = mix(value, model.omitted);
    for (const Node& node : model.nodes) {
        value = mix(value, node.identity);
        value = mix(value, static_cast<std::uint64_t>(node.lane));
    }
    for (const Edge& edge : model.edges) {
        value = mix(value, edge.source);
        value = mix(value, edge.target);
        value = mix(value, static_cast<std::uint64_t>(edge.kind));
        value = mix(value, edge.occurrenceCount);
        value = mix(value, edge.nameHash);
        value = mix(value, static_cast<std::uint64_t>(static_cast<std::int64_t>(edge.selector)));
    }
    return value == 0 ? 1 : value;
}

} // namespace

bool belongs_to_scope(const inspection::Node& node, const ActivityScope& scope) noexcept {
    if (!scope.available) {
        return false;
    }
    if (scope.preview) {
        return node.source.authoredPreview && node.source.scenarioTag.has_value()
               && *node.source.scenarioTag == scope.scenarioTag;
    }
    return !node.source.authoredPreview && node.source.activitySession.has_value()
           && *node.source.activitySession == scope.activitySession
           && node.source.scenarioTag.has_value() && *node.source.scenarioTag == scope.scenarioTag;
}

Model build(const inspection::Graph& graph,
            const std::unordered_set<std::uint64_t>& eligible,
            const ActivityScope& scope,
            std::size_t maximumNodes,
            inspection::NodeId selected) {
    Model output{};
    output.scope = scope;
    if (!scope.available || maximumNodes == 0) {
        output.revision = model_revision(output);
        return output;
    }

    std::vector<const inspection::Node*> candidates;
    candidates.reserve(eligible.size());
    std::unordered_set<inspection::NodeKey, inspection::NodeKeyHash> relationTargets;
    relationTargets.reserve(eligible.size() / 4U + 1U);
    for (const inspection::Node& node : graph.nodes()) {
        if (!eligible.contains(node.id.value) || !belongs_to_scope(node, scope)) {
            continue;
        }
        for (const inspection::Relation& relation : node.relations) {
            if (relation.kind == inspection::RelationKind::authoredLink
                || relation.kind == inspection::RelationKind::logicVariableRead
                || relation.kind == inspection::RelationKind::logicVariableWrite) {
                relationTargets.insert(relation.target);
            }
        }
    }
    std::unordered_map<std::uint64_t, std::size_t> admittedDefinitionsByParent;
    admittedDefinitionsByParent.reserve(eligible.size() / 4U + 1U);
    std::unordered_map<std::uint64_t, std::size_t> admittedPlacementsByParent;
    admittedPlacementsByParent.reserve(eligible.size() / 8U + 1U);
    std::unordered_set<std::uint64_t> admittedNodes;
    admittedNodes.reserve(eligible.size());
    for (const inspection::Node& node : graph.nodes()) {
        if (!eligible.contains(node.id.value) || !belongs_to_scope(node, scope)) {
            continue;
        }
        const bool protectedLeaf = node.id == selected || has_priority_relation(node)
                                   || relationTargets.contains(node.key);
        const inspection::Node* parent = graph.node(node.parent);
        const bool scopedParent = parent != nullptr && eligible.contains(parent->id.value)
                                  && belongs_to_scope(*parent, scope);
        if (!protectedLeaf && scopedParent) {
            if (node.kind == inspection::NodeKind::logicPlacement) {
                std::size_t& siblings = admittedPlacementsByParent[node.parent.value];
                if (!admittedNodes.contains(node.parent.value)
                    || siblings >= kPlacementSiblingLimit) {
                    ++output.compacted;
                    continue;
                }
                ++siblings;
            }
            else if (node.kind == inspection::NodeKind::logicEntity
                     && node.activityLogicMetadata.has_value()) {
                std::size_t& siblings = admittedDefinitionsByParent[node.parent.value];
                if (siblings >= kDefinitionSiblingLimit) {
                    ++output.compacted;
                    continue;
                }
                ++siblings;
            } else if (node_priority(node) == 3U) {
                ++output.compacted;
                continue;
            }
        }
        candidates.push_back(&node);
        admittedNodes.insert(node.id.value);
    }
    const auto priority = [&](const inspection::Node& node) {
        if (node.id == selected) {
            return 0U;
        }
        const unsigned base = node_priority(node);
        return base == 0U || has_priority_relation(node) || relationTargets.contains(node.key)
                   ? 1U
                   : base + 1U;
    };
    std::ranges::stable_sort(candidates, [&](const auto* left, const auto* right) {
        const unsigned leftPriority = priority(*left);
        const unsigned rightPriority = priority(*right);
        return leftPriority != rightPriority ? leftPriority < rightPriority
                                             : left->id.value < right->id.value;
    });
    if (candidates.size() > maximumNodes) {
        output.omitted = candidates.size() - maximumNodes;
        candidates.resize(maximumNodes);
    }
    std::ranges::stable_sort(candidates, [](const auto* left, const auto* right) {
        const std::uint64_t leftIdentity = node_identity(*left);
        const std::uint64_t rightIdentity = node_identity(*right);
        return leftIdentity != rightIdentity ? leftIdentity < rightIdentity
                                             : left->id.value < right->id.value;
    });

    output.nodes.reserve(candidates.size() + 1U);
    output.nodes.push_back(Node{{}, kHubIdentity, Lane::context, true});
    std::unordered_map<std::uint64_t, std::size_t> nodeIndex;
    nodeIndex.reserve(candidates.size() * 2U + 1U);
    for (const inspection::Node* node : candidates) {
        const std::size_t index = output.nodes.size();
        output.nodes.push_back(
            Node{node->id, node_identity(*node), node_lane(*node), false});
        nodeIndex.emplace(node->id.value, index);
        if (node->kind == inspection::NodeKind::logicEntity
            && node->activityLogicMetadata.has_value()) {
            ++output.definitionCount;
        } else if (node->kind == inspection::NodeKind::logicVariable) {
            ++output.variableCount;
            output.unresolvedNameCount += node->status == inspection::Status::unknownSemantic ? 1U
                                                                                              : 0U;
        }
    }

    std::unordered_set<EdgeKey, EdgeKeyHash> edges;
    edges.reserve(output.nodes.size() * 2U + 1U);
    const auto append = [&](std::size_t source,
                            std::size_t target,
                            EdgeKind kind,
                            std::uint32_t occurrenceCount,
                            std::uint32_t nameHash,
                            std::int32_t selector) {
        if (source == target || source >= output.nodes.size() || target >= output.nodes.size()) {
            return;
        }
        const EdgeKey key{output.nodes[source].identity,
                          output.nodes[target].identity,
                          kind,
                          (std::max)(occurrenceCount, 1U),
                          nameHash,
                          selector};
        if (edges.insert(key).second) {
            output.edges.push_back(
                Edge{source, target, kind, key.occurrenceCount, nameHash, selector});
        }
    };

    std::unordered_map<std::uint32_t, std::size_t> graphHashes;
    graphHashes.reserve(candidates.size());
    for (const inspection::Node* node : candidates) {
        const std::size_t index = nodeIndex.at(node->id.value);
        if (node->activityMetadata.has_value() && node->kind == inspection::NodeKind::activityGraph
            && node->activityMetadata->graphHash != 0) {
            graphHashes.try_emplace(node->activityMetadata->graphHash, index);
        }
        const auto parent = nodeIndex.find(node->parent.value);
        if (parent == nodeIndex.end()) {
            append(0, index, EdgeKind::ownership, 1, 0, -1);
        } else {
            append(parent->second, index, EdgeKind::ownership, 1, 0, -1);
        }
    }

    for (const inspection::Node* node : candidates) {
        const std::size_t index = nodeIndex.at(node->id.value);
        if (node->activityMetadata.has_value() && node->kind == inspection::NodeKind::activityGraph) {
            for (const std::uint32_t linkedHash : node->activityMetadata->linkedGraphHashes) {
                const auto target = graphHashes.find(linkedHash);
                if (target != graphHashes.end()) {
                    append(index, target->second, EdgeKind::activityGraphLink, 1, 0, -1);
                }
            }
        }
        // Placements carry copies of their definition's relationships. Their ownership edge already
        // connects them to that definition, so drawing the copies would multiply identical evidence.
        if (node->kind == inspection::NodeKind::logicPlacement) {
            continue;
        }
        for (const inspection::Relation& relation : node->relations) {
            const inspection::Node* other = graph.node(relation.target);
            if (other == nullptr || other->kind == inspection::NodeKind::logicPlacement) {
                continue;
            }
            const auto target = nodeIndex.find(other->id.value);
            if (target == nodeIndex.end()) {
                continue;
            }
            const std::size_t sourceIndex = relation.outgoing ? index : target->second;
            const std::size_t targetIndex = relation.outgoing ? target->second : index;
            append(sourceIndex,
                   targetIndex,
                   edge_kind(relation.kind),
                   relation.occurrenceCount,
                   relationship_name_hash(*node, *other, relation),
                   relation.selector);
        }
    }

    std::ranges::stable_sort(output.edges, [](const Edge& left, const Edge& right) {
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        if (left.source != right.source) {
            return left.source < right.source;
        }
        if (left.target != right.target) {
            return left.target < right.target;
        }
        if (left.nameHash != right.nameHash) {
            return left.nameHash < right.nameHash;
        }
        if (left.selector != right.selector) {
            return left.selector < right.selector;
        }
        return left.occurrenceCount < right.occurrenceCount;
    });
    output.variableAccessEdgeCount = static_cast<std::size_t>(std::ranges::count_if(
        output.edges,
        [](const Edge& edge) {
            return edge.kind == EdgeKind::logicVariableRead
                   || edge.kind == EdgeKind::logicVariableWrite;
        }));
    output.revision = model_revision(output);
    return output;
}

void layout(const Model& model, std::vector<std::array<float, 2>>& positions) {
    positions.assign(model.nodes.size(), {});
    if (model.nodes.empty()) {
        return;
    }
    constexpr float kPi = 3.14159265358979323846F;
    constexpr std::size_t kIterations = 96;
    constexpr float kRepulsion = 9200.0F;
    constexpr float kSpringStrength = 0.018F;
    constexpr float kGravity = 0.0035F;
    constexpr float kDamping = 0.82F;
    constexpr float kMaximumStep = 15.0F;

    // Stable identity-derived seeds keep repeated layouts identical while allowing the graph to
    // settle as one connected constellation instead of imposing semantic columns.
    for (std::size_t index = 1; index < model.nodes.size(); ++index) {
        std::uint64_t seed = model.nodes[index].identity;
        seed ^= seed >> 30U;
        seed *= 0xBF58476D1CE4E5B9ULL;
        seed ^= seed >> 27U;
        seed *= 0x94D049BB133111EBULL;
        seed ^= seed >> 31U;
        const float angle = static_cast<float>(seed & 0xFFFFU) / 65535.0F * 2.0F * kPi;
        const float radius = 170.0F
                             + static_cast<float>((seed >> 16U) & 0xFFFFU) / 65535.0F * 250.0F;
        positions[index] = {std::cos(angle) * radius, std::sin(angle) * radius};
    }

    std::vector<std::array<float, 2>> velocity(model.nodes.size());
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        for (std::size_t left = 0; left < model.nodes.size(); ++left) {
            for (std::size_t right = left + 1U; right < model.nodes.size(); ++right) {
                float dx = positions[left][0] - positions[right][0];
                float dy = positions[left][1] - positions[right][1];
                float distanceSquared = dx * dx + dy * dy;
                if (distanceSquared < 1.0F) {
                    const std::uint64_t pair = model.nodes[left].identity ^ model.nodes[right].identity;
                    const float angle = static_cast<float>(pair & 0xFFFFU) / 65535.0F * 2.0F * kPi;
                    dx = std::cos(angle);
                    dy = std::sin(angle);
                    distanceSquared = 1.0F;
                }
                const float distance = std::sqrt(distanceSquared);
                const float force = (std::min)(kRepulsion / distanceSquared, 7.0F);
                const float forceX = dx / distance * force;
                const float forceY = dy / distance * force;
                velocity[left][0] += forceX;
                velocity[left][1] += forceY;
                velocity[right][0] -= forceX;
                velocity[right][1] -= forceY;
            }
        }

        for (const Edge& edge : model.edges) {
            if (edge.source >= model.nodes.size() || edge.target >= model.nodes.size()) {
                continue;
            }
            const float dx = positions[edge.target][0] - positions[edge.source][0];
            const float dy = positions[edge.target][1] - positions[edge.source][1];
            const float distance = (std::max)(std::sqrt(dx * dx + dy * dy), 1.0F);
            const float desired = edge.kind == EdgeKind::ownership ? 105.0F : 145.0F;
            const float force = (distance - desired) * kSpringStrength;
            const float forceX = dx / distance * force;
            const float forceY = dy / distance * force;
            velocity[edge.source][0] += forceX;
            velocity[edge.source][1] += forceY;
            velocity[edge.target][0] -= forceX;
            velocity[edge.target][1] -= forceY;
        }

        for (std::size_t index = 1; index < model.nodes.size(); ++index) {
            velocity[index][0] -= positions[index][0] * kGravity;
            velocity[index][1] -= positions[index][1] * kGravity;
            velocity[index][0] *= kDamping;
            velocity[index][1] *= kDamping;
            const float speed = std::sqrt(velocity[index][0] * velocity[index][0]
                                          + velocity[index][1] * velocity[index][1]);
            if (speed > kMaximumStep) {
                velocity[index][0] *= kMaximumStep / speed;
                velocity[index][1] *= kMaximumStep / speed;
            }
            positions[index][0] += velocity[index][0];
            positions[index][1] += velocity[index][1];
        }
        positions[0] = {};
        velocity[0] = {};
    }
}

} // namespace sunrise::client::ui::world_inspector::overview
