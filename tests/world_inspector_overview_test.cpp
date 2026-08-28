#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

#include "client/ui/world_inspector/world_inspector_overview.h"

namespace inspection = sunrise::client::inspection;
namespace overview = sunrise::client::ui::world_inspector::overview;

namespace {

inspection::Node make_node(const char* name,
                           inspection::NodeKind kind,
                           std::uint64_t nativeKey,
                           bool preview,
                           std::uint32_t scenario) {
    inspection::Node node{};
    node.name = name;
    node.kind = kind;
    node.producer = kind == inspection::NodeKind::activityGraph
                        ? inspection::Producer::activityCatalog
                        : inspection::Producer::activityLogicCatalog;
    node.provenance = inspection::Provenance::catalog;
    node.nativeKey = nativeKey;
    node.source.authoredPreview = preview;
    node.source.scenarioTag = scenario;
    if (!preview) {
        node.source.activitySession = 123;
    }
    return node;
}

std::size_t edge_count(const overview::Model& model, overview::EdgeKind kind) {
    std::size_t result = 0;
    for (const overview::Edge& edge : model.edges) {
        result += edge.kind == kind ? 1U : 0U;
    }
    return result;
}

} // namespace

int main() {
    constexpr std::uint32_t kPreviewScenario = 0x80000001U;
    inspection::Graph graph;
    graph.reset(1);

    inspection::Node liveRoot = make_node(
        "live", inspection::NodeKind::activity, 1, false, 0x80000002U);
    liveRoot.producer = inspection::Producer::graph;
    const inspection::NodeId liveId = graph.add(std::move(liveRoot));

    inspection::Node graphA = make_node(
        "graph a", inspection::NodeKind::activityGraph, 2, true, kPreviewScenario);
    graphA.activityMetadata = inspection::ActivityMetadata{};
    graphA.activityMetadata->graphHash = 0x81000001U;
    graphA.activityMetadata->linkedGraphHashes = {0x81000002U};
    const inspection::NodeId graphAId = graph.add(std::move(graphA), liveId);

    inspection::Node graphB = make_node(
        "graph b", inspection::NodeKind::activityGraph, 3, true, kPreviewScenario);
    graphB.activityMetadata = inspection::ActivityMetadata{};
    graphB.activityMetadata->graphHash = 0x81000002U;
    const inspection::NodeId graphBId = graph.add(std::move(graphB), liveId);

    inspection::Node entityA = make_node(
        "entity a", inspection::NodeKind::logicEntity, 4, true, kPreviewScenario);
    entityA.activityLogicMetadata = inspection::ActivityLogicMetadata{};
    entityA.activityLogicMetadata->definitionTag = 0x82000001U;
    const inspection::NodeId entityAId = graph.add(std::move(entityA), liveId);

    inspection::Node entityB = make_node(
        "entity b", inspection::NodeKind::logicEntity, 5, true, kPreviewScenario);
    entityB.activityLogicMetadata = inspection::ActivityLogicMetadata{};
    entityB.activityLogicMetadata->definitionTag = 0x82000002U;
    const inspection::NodeId entityBId = graph.add(std::move(entityB), liveId);

    inspection::Node placement = make_node(
        "placement", inspection::NodeKind::logicPlacement, 6, true, kPreviewScenario);
    placement.activityLogicMetadata = inspection::ActivityLogicMetadata{};
    placement.activityLogicMetadata->definitionTag = 0x82000001U;
    const inspection::NodeId placementId = graph.add(std::move(placement), entityAId);

    inspection::Node foreign = make_node(
        "foreign preview", inspection::NodeKind::activityGraph, 7, true, 0x80000003U);
    foreign.activityMetadata = inspection::ActivityMetadata{};
    foreign.activityMetadata->graphHash = 0x81000003U;
    const inspection::NodeId foreignId = graph.add(std::move(foreign), liveId);

    inspection::Node untagged = make_node(
        "untagged preview", inspection::NodeKind::activityGraph, 8, true, kPreviewScenario);
    untagged.source.scenarioTag.reset();
    untagged.activityMetadata = inspection::ActivityMetadata{};
    untagged.activityMetadata->graphHash = 0x81000004U;
    const inspection::NodeId untaggedId = graph.add(std::move(untagged), liveId);

    inspection::Node* source = graph.node(entityAId);
    inspection::Node* target = graph.node(entityBId);
    inspection::Node* placementNode = graph.node(placementId);
    if (source == nullptr || target == nullptr || placementNode == nullptr) {
        return 1;
    }
    source->activityLogicMetadata->relationships.push_back(
        {0x82000002U, 0x83000001U, 3, true});
    target->activityLogicMetadata->relationships.push_back(
        {0x82000001U, 0x83000001U, 3, false});
    placementNode->activityLogicMetadata->relationships =
        source->activityLogicMetadata->relationships;
    source->relations.push_back(
        {target->key, inspection::RelationKind::logic, inspection::Provenance::catalog, 3, true});
    target->relations.push_back(
        {source->key, inspection::RelationKind::logic, inspection::Provenance::catalog, 3, false});
    placementNode->relations.push_back(
        {target->key, inspection::RelationKind::logic, inspection::Provenance::catalog, 3, true});

    std::unordered_set<std::uint64_t> eligible;
    for (const inspection::Node& node : graph.nodes()) {
        eligible.insert(node.id.value);
    }
    const overview::ActivityScope preview{"preview", 0, kPreviewScenario, true, true};
    const overview::Model model = overview::build(graph, eligible, preview, 32);
    if (model.nodes.size() != 6U || model.compacted != 0U || model.omitted != 0
        || model.definitionCount != 2U || model.variableCount != 0U
        || model.variableAccessEdgeCount != 0U || model.unresolvedNameCount != 0U
        || edge_count(model, overview::EdgeKind::logic) != 1U
        || edge_count(model, overview::EdgeKind::activityGraphLink) != 1U) {
        return 2;
    }
    for (const overview::Node& node : model.nodes) {
        if (node.source == liveId || node.source == foreignId || node.source == untaggedId) {
            return 3;
        }
    }

    std::vector<std::array<float, 2>> first;
    std::vector<std::array<float, 2>> second;
    overview::layout(model, first);
    overview::layout(model, second);
    if (first != second || first.size() != model.nodes.size()) {
        return 4;
    }
    for (const auto& position : first) {
        if (!std::isfinite(position[0]) || !std::isfinite(position[1])) {
            return 5;
        }
    }

    const overview::Model limited = overview::build(graph, eligible, preview, 2);
    if (limited.nodes.size() != 3U || limited.omitted != 3U) {
        return 6;
    }
    const std::unordered_set<std::uint64_t> childOnly{placementId.value};
    const overview::Model filtered = overview::build(graph, childOnly, preview, 32);
    if (filtered.nodes.size() != 2U || filtered.nodes[1].source != placementId
        || filtered.edges.size() != 1U
        || filtered.edges[0].kind != overview::EdgeKind::ownership) {
        return 7;
    }
    const overview::ActivityScope live{"live", 123, 0x80000002U, true, false};
    const overview::Model liveModel = overview::build(graph, eligible, live, 32);
    if (liveModel.nodes.size() != 2U || liveModel.nodes[1].source != liveId) {
        return 8;
    }
    inspection::NodeId lastLeaf{};
    for (std::uint64_t index = 0; index < 60U; ++index) {
        inspection::Node leaf = make_node("runtime leaf",
                                          inspection::NodeKind::runtimeEntity,
                                          100U + index,
                                          false,
                                          0x80000002U);
        leaf.producer = inspection::Producer::objectSystem;
        const inspection::NodeId id = graph.add(std::move(leaf), liveId);
        if (!id) {
            return 9;
        }
        lastLeaf = id;
        eligible.insert(id.value);
    }
    const overview::Model compacted = overview::build(graph, eligible, live, 100);
    if (compacted.nodes.size() != 2U || compacted.compacted != 60U || compacted.omitted != 0U) {
        return 10;
    }
    const overview::Model pinned = overview::build(graph, eligible, live, 100, lastLeaf);
    const bool selectedPresent = std::ranges::any_of(pinned.nodes, [lastLeaf](const auto& node) {
        return node.source == lastLeaf;
    });
    if (!selectedPresent || pinned.nodes.size() != 3U || pinned.compacted != 59U) {
        return 11;
    }
    if (graphAId == graphBId) {
        return 12;
    }

    inspection::Node logicRoot = make_node(
        "logic root", inspection::NodeKind::logicGroup, 0x90000001U, true, kPreviewScenario);
    logicRoot.tag = 0x90000001U;
    logicRoot.classHash = 0x8080941EU;
    const inspection::NodeId logicRootId = graph.add(std::move(logicRoot), liveId);
    inspection::Node variable = make_node(
        "stack count", inspection::NodeKind::logicVariable, 0x90000002U, true, kPreviewScenario);
    variable.tag = 0x90000002U;
    variable.nameHash = 0x90000003U;
    const inspection::NodeId variableId = graph.add(std::move(variable), liveId);
    if (!logicRootId || !variableId) {
        return 13;
    }
    inspection::Node* rootNode = graph.node(logicRootId);
    inspection::Node* variableNode = graph.node(variableId);
    rootNode->relations.push_back({variableNode->key,
                                   inspection::RelationKind::logicVariableRead,
                                   inspection::Provenance::catalog,
                                   4,
                                   false,
                                   0x90000003U,
                                   7});
    variableNode->relations.push_back({rootNode->key,
                                       inspection::RelationKind::logicVariableRead,
                                       inspection::Provenance::catalog,
                                       4,
                                       true,
                                       0x90000003U,
                                       7});
    inspection::Node owner = make_node(
        "variable owner", inspection::NodeKind::logicEntity, 0x90000004U, true, kPreviewScenario);
    owner.tag = 0x90000004U;
    owner.classHash = 0x80809C0FU;
    const inspection::NodeId ownerId = graph.add(std::move(owner), liveId);
    inspection::Node* ownerNode = graph.node(ownerId);
    if (!ownerId || ownerNode == nullptr) {
        return 20;
    }
    ownerNode->relations.push_back({variableNode->key,
                                    inspection::RelationKind::authoredLink,
                                    inspection::Provenance::catalog,
                                    1,
                                    true});
    variableNode->relations.push_back({ownerNode->key,
                                       inspection::RelationKind::authoredLink,
                                       inspection::Provenance::catalog,
                                       1,
                                       false});
    eligible.insert(logicRootId.value);
    eligible.insert(variableId.value);
    eligible.insert(ownerId.value);
    const overview::Model variableModel = overview::build(graph, eligible, preview, 64);
    const auto variableEdge = std::ranges::find_if(
        variableModel.edges, [](const overview::Edge& edge) {
            return edge.kind == overview::EdgeKind::logicVariableRead;
        });
    if (variableEdge == variableModel.edges.end() || variableEdge->occurrenceCount != 4
        || variableEdge->nameHash != 0x90000003U || variableEdge->selector != 7
        || variableModel.definitionCount != 2U || variableModel.variableCount != 1U
        || variableModel.variableAccessEdgeCount != 1U) {
        return 14;
    }
    const auto rootVisual = std::ranges::find_if(variableModel.nodes, [logicRootId](const auto& node) {
        return node.source == logicRootId;
    });
    const auto variableVisual =
        std::ranges::find_if(variableModel.nodes, [variableId](const auto& node) {
            return node.source == variableId;
        });
    const auto ownerVisual = std::ranges::find_if(variableModel.nodes, [ownerId](const auto& node) {
        return node.source == ownerId;
    });
    if (rootVisual == variableModel.nodes.end() || variableVisual == variableModel.nodes.end()
        || ownerVisual == variableModel.nodes.end()
        || rootVisual->lane != overview::Lane::behaviorRoot
        || variableVisual->lane != overview::Lane::variable
        || ownerVisual->lane != overview::Lane::stateVarOwner) {
        return 18;
    }
    std::vector<std::array<float, 2>> variablePositions;
    overview::layout(variableModel, variablePositions);
    const std::size_t rootIndex = static_cast<std::size_t>(rootVisual - variableModel.nodes.begin());
    const std::size_t variableIndex =
        static_cast<std::size_t>(variableVisual - variableModel.nodes.begin());
    const std::size_t ownerIndex =
        static_cast<std::size_t>(ownerVisual - variableModel.nodes.begin());
    const auto separated = [](const std::array<float, 2>& left,
                              const std::array<float, 2>& right) {
        const float dx = left[0] - right[0];
        const float dy = left[1] - right[1];
        return dx * dx + dy * dy > 1.0F;
    };
    if (variablePositions.empty() || variablePositions[0] != std::array<float, 2>{}
        || !separated(variablePositions[rootIndex], variablePositions[variableIndex])
        || !separated(variablePositions[variableIndex], variablePositions[ownerIndex])) {
        return 19;
    }
    const overview::Model selectedVariableModel =
        overview::build(graph, eligible, preview, 64, variableId);
    if (selectedVariableModel.revision != variableModel.revision
        || selectedVariableModel.nodes.size() != variableModel.nodes.size()) {
        return 20;
    }
    for (std::size_t index = 0; index < variableModel.nodes.size(); ++index) {
        if (selectedVariableModel.nodes[index].identity != variableModel.nodes[index].identity) {
            return 20;
        }
    }
    for (std::uint64_t index = 0; index < 8U; ++index) {
        inspection::Node context = make_node("unrelated context",
                                             inspection::NodeKind::runtimeEntity,
                                             0x91000000U + index,
                                             true,
                                             kPreviewScenario);
        const inspection::NodeId contextId = graph.add(std::move(context), liveId);
        if (!contextId) {
            return 21;
        }
        eligible.insert(contextId.value);
    }
    const overview::Model withContext = overview::build(graph, eligible, preview, 64);
    std::vector<std::array<float, 2>> withContextPositions;
    overview::layout(withContext, withContextPositions);
    const auto position_for = [&](inspection::NodeId id) {
        const auto found = std::ranges::find_if(withContext.nodes, [id](const auto& node) {
            return node.source == id;
        });
        return found == withContext.nodes.end()
                   ? std::array<float, 2>{(std::numeric_limits<float>::quiet_NaN)(), 0.0F}
                   : withContextPositions[static_cast<std::size_t>(found - withContext.nodes.begin())];
    };
    const auto rootWithContext = position_for(logicRootId);
    const auto variableWithContext = position_for(variableId);
    const auto ownerWithContext = position_for(ownerId);
    if (!std::isfinite(rootWithContext[0]) || !std::isfinite(rootWithContext[1])
        || !std::isfinite(variableWithContext[0]) || !std::isfinite(variableWithContext[1])
        || !std::isfinite(ownerWithContext[0]) || !std::isfinite(ownerWithContext[1])
        || !separated(rootWithContext, variableWithContext)
        || !separated(variableWithContext, ownerWithContext)) {
        return 22;
    }

    inspection::Graph copiedRelationGraph;
    copiedRelationGraph.reset(2);
    const inspection::NodeId copiedParent = copiedRelationGraph.add(make_node(
        "definition", inspection::NodeKind::logicEntity, 0xA0000001U, true, kPreviewScenario));
    const inspection::NodeId copiedTarget = copiedRelationGraph.add(make_node(
        "target", inspection::NodeKind::logicEntity, 0xA0000002U, true, kPreviewScenario));
    if (!copiedParent || !copiedTarget) {
        return 15;
    }
    std::unordered_set<std::uint64_t> copiedEligible{copiedParent.value, copiedTarget.value};
    for (std::uint64_t index = 0; index < 12U; ++index) {
        inspection::Node copied = make_node("placement",
                                            inspection::NodeKind::logicPlacement,
                                            0xA0000100U + index,
                                            true,
                                            kPreviewScenario);
        copied.relations.push_back({copiedRelationGraph.node(copiedTarget)->key,
                                    inspection::RelationKind::logic,
                                    inspection::Provenance::catalog,
                                    1,
                                    true});
        const inspection::NodeId copiedId = copiedRelationGraph.add(std::move(copied), copiedParent);
        if (!copiedId) {
            return 16;
        }
        copiedEligible.insert(copiedId.value);
    }
    const overview::Model copiedModel =
        overview::build(copiedRelationGraph, copiedEligible, preview, 64);
    if (copiedModel.nodes.size() != 4U || copiedModel.compacted != 11U) {
        return 17;
    }

    overview::Model fullLayout;
    fullLayout.nodes.push_back({{}, 1U, overview::Lane::context, true});
    for (std::size_t index = 1; index <= 320U; ++index) {
        fullLayout.nodes.push_back(
            {{}, 0xB0000000U + index, overview::Lane::context, false});
        fullLayout.edges.push_back(
            {index == 1U ? 0U : index / 2U, index, overview::EdgeKind::ownership});
    }
    std::vector<std::array<float, 2>> fullPositions;
    overview::layout(fullLayout, fullPositions);
    if (fullPositions.size() != fullLayout.nodes.size()) {
        return 23;
    }
    float minimumX = (std::numeric_limits<float>::max)();
    float minimumY = (std::numeric_limits<float>::max)();
    float maximumX = (std::numeric_limits<float>::lowest)();
    float maximumY = (std::numeric_limits<float>::lowest)();
    for (const auto& position : fullPositions) {
        if (!std::isfinite(position[0]) || !std::isfinite(position[1])) {
            return 24;
        }
        minimumX = (std::min)(minimumX, position[0]);
        minimumY = (std::min)(minimumY, position[1]);
        maximumX = (std::max)(maximumX, position[0]);
        maximumY = (std::max)(maximumY, position[1]);
    }
    if (maximumX - minimumX < 500.0F || maximumY - minimumY < 500.0F) {
        return 25;
    }
    return 0;
}
