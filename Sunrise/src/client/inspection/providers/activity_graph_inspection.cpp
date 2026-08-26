#include "activity_graph_inspection.h"

#include <array>
#include <cstdio>
#include <utility>

namespace sunrise::client::inspection::providers::activity_graph {
namespace {

State g_state{};

[[nodiscard]] std::string hash_label(const char* prefix, std::uint32_t hash) {
    std::array<char, 64> text{};
    const int written = std::snprintf(text.data(), text.size(), "%s 0x%08X", prefix, hash);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string(prefix);
}

[[nodiscard]] std::uint64_t node_key(std::uint32_t graphHash, std::uint32_t nodeHash) noexcept {
    return (static_cast<std::uint64_t>(graphHash) << 32U) | nodeHash;
}

[[nodiscard]] std::uint64_t reference_key(std::uint32_t graphHash,
                                          std::uint32_t nodeHash,
                                          std::uint32_t activityHash) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (const std::uint32_t lane : {graphHash, nodeHash, activityHash}) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value ^= static_cast<std::uint8_t>(lane >> shift);
            value *= 1099511628211ULL;
        }
    }
    return value == 0 ? 1 : value;
}

[[nodiscard]] ActivityMetadata metadata_for(const activity_catalog::Catalog& catalog,
                                            std::uint32_t graphHash,
                                            std::uint32_t nodeHash,
                                            std::uint32_t activityHash,
                                            std::array<float, 2> position,
                                            std::uint32_t referenceCount) {
    ActivityMetadata metadata{};
    metadata.activityHash = activityHash;
    metadata.graphHash = graphHash;
    metadata.nodeHash = nodeHash;
    metadata.authoredPosition = position;
    metadata.referenceCount = referenceCount;
    metadata.catalogBuild = catalog.contentBuild;
    metadata.collectorVersion = catalog.collectorVersion;
    if (const activity_catalog::Graph* graph = activity_catalog::find_graph(catalog, graphHash);
        graph != nullptr) {
        if (const activity_catalog::GraphNode* node = activity_catalog::find_node(*graph, nodeHash);
            node != nullptr) {
            metadata.nativeStateValues = node->stateValues;
            metadata.linkedGraphHashes = node->linkedGraphHashes;
        }
    }
    return metadata;
}

} // namespace

const State& state() noexcept {
    return g_state;
}

bool activate_location(activity_catalog::Catalog catalog, Source source) noexcept {
    try {
        std::string error;
        if (!source.scenarioTag.has_value() || *source.scenarioTag == 0
            || activity_catalog::compatibility(catalog)
                   != activity_catalog::Compatibility::compatible
            || !activity_catalog::validate(catalog, error)) {
            return false;
        }
        g_state.locationCatalog = std::move(catalog);
        g_state.locationScenarioTag = *source.scenarioTag;
        g_state.activationSource = std::move(source);
        g_state.locationActive = true;
        ++g_state.publicationRevision;
        if (g_state.publicationRevision == 0) {
            g_state.publicationRevision = 1;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void deactivate_location() noexcept {
    if (!g_state.locationActive) {
        return;
    }
    g_state.locationCatalog = {};
    g_state.locationScenarioTag = 0;
    g_state.activationSource = {};
    g_state.locationActive = false;
    ++g_state.publicationRevision;
    if (g_state.publicationRevision == 0) {
        g_state.publicationRevision = 1;
    }
}

std::uint64_t publication_revision() noexcept {
    return g_state.publicationRevision;
}

AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent) {
    AppendResult result{};
    const bool locationMatch =
        g_state.locationActive
        && (g_state.activationSource.authoredPreview
            || (source.scenarioTag.has_value() && *source.scenarioTag == g_state.locationScenarioTag));
    result.present = locationMatch;
    result.diagnostic = locationMatch
                            ? (g_state.activationSource.authoredPreview
                                   ? "Authored Activity Graph preview loaded."
                                   : "Current-location Activity Graph cache loaded.")
                                      : "No matching current-location Activity Graph is active.";
    if (!result.present) {
        return result;
    }
    const activity_catalog::Catalog& catalog = g_state.locationCatalog;
    const Source& evidenceSource =
        g_state.activationSource.authoredPreview ? g_state.activationSource : source;
    result.contentBuild = catalog.contentBuild;
    result.collectorVersion = catalog.collectorVersion;

    Node catalogNode;
    catalogNode.name = evidenceSource.authoredPreview && !evidenceSource.packageName.empty()
                           ? "Activity graph / " + evidenceSource.packageName
                           : "Activity graph";
    catalogNode.searchText = "activity graph package authored positions "
                             + evidenceSource.packageName;
    catalogNode.kind = NodeKind::activityGraph;
    catalogNode.status = Status::known;
    catalogNode.producer = Producer::activityCatalog;
    catalogNode.provenance = Provenance::catalog;
    catalogNode.nativeKey = result.contentBuild;
    catalogNode.source = evidenceSource;
    catalogNode.actions = Action::copyId;
    catalogNode.activityMetadata = ActivityMetadata{};
    catalogNode.activityMetadata->catalogBuild = result.contentBuild;
    catalogNode.activityMetadata->collectorVersion = result.collectorVersion;
    result.catalogNode = graph.add(std::move(catalogNode), parent);
    if (!result.catalogNode) {
        diagnostics.push_back({Diagnostic::Severity::error,
                               "The inspection graph could not create the activity catalog node."});
        return result;
    }
    for (const activity_catalog::Graph& sourceGraph : catalog.graphs) {
        Node graphNode;
        graphNode.name = hash_label("Activity graph", sourceGraph.hash);
        graphNode.searchText = "activity graph authored positions linked graph package";
        graphNode.kind = NodeKind::activityGraph;
        graphNode.status = Status::known;
        graphNode.producer = Producer::activityCatalog;
        graphNode.provenance = Provenance::catalog;
        graphNode.nativeKey = sourceGraph.hash;
        graphNode.source = evidenceSource;
        graphNode.actions = Action::copyId;
        graphNode.activityMetadata = ActivityMetadata{};
        graphNode.activityMetadata->graphHash = sourceGraph.hash;
        graphNode.activityMetadata->linkedGraphHashes = sourceGraph.linkedGraphHashes;
        graphNode.activityMetadata->catalogBuild = result.contentBuild;
        graphNode.activityMetadata->collectorVersion = result.collectorVersion;
        const NodeId graphId = graph.add(std::move(graphNode), result.catalogNode);
        if (!graphId) {
            diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph reached its node-id capacity for activity graphs."});
            break;
        }

        for (const activity_catalog::GraphNode& sourceNode : sourceGraph.nodes) {
            Node node;
            node.name = hash_label("Graph node", sourceNode.nodeHash);
            node.searchText = "activity graph node authored position native state hash";
            node.kind = NodeKind::activityGraphNode;
            node.status = Status::known;
            node.producer = Producer::activityCatalog;
            node.provenance = Provenance::catalog;
            node.nativeKey = node_key(sourceGraph.hash, sourceNode.nodeHash);
            node.source = evidenceSource;
            node.actions = Action::copyId;
            node.activityMetadata =
                metadata_for(catalog,
                             sourceGraph.hash,
                             sourceNode.nodeHash,
                             0,
                             {sourceNode.authoredX, sourceNode.authoredY},
                             static_cast<std::uint32_t>(sourceNode.activityHashes.size()));
            const NodeId nodeId = graph.add(std::move(node), graphId);
            if (!nodeId) {
                diagnostics.push_back(
                    {Diagnostic::Severity::error,
                     "The inspection graph reached its node-id capacity for activity nodes."});
                break;
            }
            for (const std::uint32_t activityHash : sourceNode.activityHashes) {
                const activity_catalog::Activity* activity =
                    activity_catalog::find_activity(catalog, activityHash);
                Node reference;
                reference.name = activity != nullptr && !activity->name.empty()
                                     ? activity->name
                                     : hash_label("Activity", activityHash);
                reference.searchText = "activity reference graph node package";
                reference.kind = NodeKind::activityReference;
                reference.status = Status::known;
                reference.producer = Producer::activityCatalog;
                reference.provenance = Provenance::catalog;
                reference.nativeKey =
                    reference_key(sourceGraph.hash, sourceNode.nodeHash, activityHash);
                reference.source = evidenceSource;
                reference.actions = Action::copyId;
                reference.activityMetadata =
                    metadata_for(catalog,
                                 sourceGraph.hash,
                                 sourceNode.nodeHash,
                                 activityHash,
                                 {sourceNode.authoredX, sourceNode.authoredY},
                                 static_cast<std::uint32_t>(sourceNode.activityHashes.size()));
                if (!graph.add(std::move(reference), nodeId)) {
                    diagnostics.push_back({Diagnostic::Severity::error,
                                           "The inspection graph reached its node-id capacity for "
                                           "activity references."});
                    break;
                }
            }
        }
    }

    diagnostics.push_back({Diagnostic::Severity::information,
                           "Activity Graph package evidence loaded for the current location."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Authored positions only; no node connections in this catalog."});
    return result;
}

} // namespace sunrise::client::inspection::providers::activity_graph
