#include <algorithm>
#include <cstdint>
#include <vector>

#include "client/inspection/providers/activity_graph_inspection.h"
#include "client/inspection/providers/activity_logic_inspection.h"

namespace inspection = sunrise::client::inspection;
namespace graph_catalog = inspection::activity_catalog;
namespace logic_catalog = inspection::activity_logic_catalog;
namespace graph_provider = inspection::providers::activity_graph;
namespace logic_provider = inspection::providers::activity_logic;

namespace {

inspection::Source preview_source() {
    inspection::Source source{};
    source.packageName = "fleet_strike";
    source.mapStem = "fleet";
    source.scenarioTag = 0x80B00001U;
    source.authoredPreview = true;
    return source;
}

graph_catalog::Catalog graph_value() {
    graph_catalog::Catalog value{};
    value.contentBuild = graph_catalog::kTargetContentBuild;
    value.collectorVersion = graph_catalog::kCollectorVersion;
    value.scenarioTag = 0x80B00001U;
    value.contentFingerprint[0] = 1;
    value.activities = {{0x80B00001U, "fleet_strike", {0x81000001U}}};
    graph_catalog::GraphNode node{};
    node.graphHash = 0x81000001U;
    node.nodeHash = 0x81000002U;
    node.stateValues = {0, 1};
    node.activityHashes = {0x80B00001U};
    value.graphs = {{0x81000001U, {node}, {}}};
    return value;
}

logic_catalog::Catalog logic_value() {
    logic_catalog::Catalog value{};
    value.provenance.contentBuild = 86657;
    value.provenance.collectorVersion = logic_catalog::kCollectorVersion;
    value.provenance.contentFingerprint[0] = 1;
    logic_catalog::Entity entity{};
    entity.definitionTag = 0x80800001U;
    entity.classPrimary = 1;
    entity.classSecondary = 2;
    entity.role = logic_catalog::Role::object;
    entity.confidence = logic_catalog::Confidence::strong;
    entity.name = "preview object";
    entity.label = "object";
    entity.placements.push_back(
        {0x80810001U, 0x80820001U, 7, {1.0F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F, 1.0F}});
    value.entities.push_back(std::move(entity));
    value.activities = {{0x80B00001U, "fleet_strike", "fleet", {0}}};
    return value;
}

bool preview_sources_are_authored(const inspection::Graph& graph) {
    bool foundGraph = false;
    bool foundPlacement = false;
    for (const inspection::Node& node : graph.nodes()) {
        if (node.producer != inspection::Producer::activityCatalog
            && node.producer != inspection::Producer::activityLogicCatalog) {
            continue;
        }
        if (!node.source.authoredPreview || node.source.packageName != "fleet_strike"
            || node.source.mapStem != "fleet" || node.source.activitySession.has_value()
            || node.source.activityIndex.has_value() || node.source.bubble.has_value()
            || node.source.spawnSetHash.has_value()) {
            return false;
        }
        foundGraph = foundGraph || node.producer == inspection::Producer::activityCatalog;
        foundPlacement = foundPlacement || node.kind == inspection::NodeKind::logicPlacement;
    }
    return foundGraph && foundPlacement;
}

} // namespace

int main() {
    inspection::Graph graph;
    graph.reset(1);
    inspection::Node root{};
    root.name = "root";
    root.kind = inspection::NodeKind::world;
    const inspection::NodeId rootId = graph.add(std::move(root));
    if (!rootId) {
        return 1;
    }

    if (!graph_provider::activate_location(graph_value(), preview_source())
        || !logic_provider::activate_location(logic_value(), preview_source())) {
        return 2;
    }
    inspection::Source live{};
    live.packageName = "fleet_freeroam";
    live.mapStem = "fleet";
    live.scenarioTag = 0x80B00002U;
    live.activitySession = 0x1234U;
    std::vector<inspection::Diagnostic> diagnostics;
    const auto graphResult = graph_provider::append(graph, diagnostics, live, rootId);
    const auto logicResult = logic_provider::append(graph, diagnostics, live, rootId);
    if (!graphResult.present || !logicResult.present || !logicResult.matched
        || !preview_sources_are_authored(graph)) {
        return 3;
    }

    const std::uint64_t graphRevision = graph_provider::publication_revision();
    const std::uint64_t logicRevision = logic_provider::publication_revision();
    graph_provider::deactivate_location();
    logic_provider::deactivate_location();
    if (graph_provider::state().locationActive || logic_provider::state().locationActive
        || graph_provider::publication_revision() == graphRevision
        || logic_provider::publication_revision() == logicRevision) {
        return 4;
    }
    return 0;
}
