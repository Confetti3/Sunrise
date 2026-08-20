#include "activity_graph_inspection.h"

#include <array>
#include <cstdio>
#include <utility>

#include "../../../core/filesystem/path.h"

namespace sunrise::client::inspection::providers::activity_graph {
namespace {

constexpr std::wstring_view kCatalogSuffix = L"\\activity-graph-catalog.bin";
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
                                            std::uint32_t referenceCount,
                                            std::uint32_t releaseCount) {
    ActivityMetadata metadata{};
    metadata.activityHash = activityHash;
    metadata.graphHash = graphHash;
    metadata.nodeHash = nodeHash;
    metadata.authoredPosition = position;
    metadata.referenceCount = referenceCount;
    metadata.releaseCount = releaseCount;
    metadata.catalogBuild = catalog.contentBuild;
    metadata.catalogVersion = catalog.manifestVersion;
    metadata.buildMatch = activity_catalog::compatibility(catalog)
                          == activity_catalog::Compatibility::compatible;
    metadata.browseOnly = !metadata.buildMatch;
    if (const activity_catalog::Graph* graph = activity_catalog::find_graph(catalog, graphHash);
        graph != nullptr) {
        if (const activity_catalog::GraphNode* node = activity_catalog::find_node(*graph, nodeHash);
            node != nullptr) {
            metadata.stateHash = node->stateHash;
            metadata.styleHash = node->styleHash;
            metadata.linkedGraphHashes = node->linkedGraphHashes;
        }
    }
    return metadata;
}

} // namespace

void initialize(void* module) noexcept {
    g_state = {};
    g_state.initialized = true;
    core::path::Buffer path{};
    if (!core::path::artifact_directory(module, path)
        || !core::path::append(path, kCatalogSuffix)) {
        g_state.load.compatibility = activity_catalog::Compatibility::missing;
        g_state.load.diagnostic = "activity catalog artifact path is unavailable";
        return;
    }
    g_state.load = activity_catalog::load_file(path.chars.data(), g_state.catalog);
}

void shutdown() noexcept {
    g_state = {};
}

const State& state() noexcept {
    return g_state;
}

AppendResult append(Graph& graph,
                    std::vector<Diagnostic>& diagnostics,
                    const Source& source,
                    NodeId parent) {
    AppendResult result{};
    result.present = g_state.load.compatibility == activity_catalog::Compatibility::compatible
                     || g_state.load.compatibility == activity_catalog::Compatibility::buildMismatch;
    result.buildMatch = g_state.load.compatibility == activity_catalog::Compatibility::compatible;
    result.contentBuild = g_state.catalog.contentBuild;
    result.version = g_state.catalog.manifestVersion;
    result.diagnostic = g_state.load.diagnostic;
    if (!result.present) {
        diagnostics.push_back({Diagnostic::Severity::information,
                               g_state.load.diagnostic.empty()
                                   ? "No optional activity catalog is installed."
                                   : g_state.load.diagnostic});
        return result;
    }

    Node catalogNode;
    catalogNode.name = result.buildMatch ? "Activity catalog" : "Activity catalog (Browse only)";
    catalogNode.searchText = "activity graph catalog authored positions browse only";
    catalogNode.kind = NodeKind::activityGraph;
    catalogNode.status = result.buildMatch ? Status::known : Status::deferred;
    catalogNode.producer = Producer::activityCatalog;
    catalogNode.provenance = Provenance::catalog;
    catalogNode.nativeKey = result.contentBuild;
    catalogNode.source = source;
    catalogNode.actions = Action::copyId;
    catalogNode.activityMetadata = ActivityMetadata{};
    catalogNode.activityMetadata->catalogBuild = result.contentBuild;
    catalogNode.activityMetadata->catalogVersion = result.version;
    catalogNode.activityMetadata->buildMatch = result.buildMatch;
    catalogNode.activityMetadata->browseOnly = !result.buildMatch;
    result.catalogNode = graph.add(std::move(catalogNode), parent);
    if (!result.catalogNode) {
        diagnostics.push_back({Diagnostic::Severity::error,
                               "The inspection graph could not create the activity catalog node."});
        return result;
    }
    const activity_catalog::Catalog& catalog = g_state.catalog;
    const auto release_count = [&catalog](std::uint32_t graphHash, std::uint32_t nodeHash) {
        std::uint32_t count = 0;
        for (const activity_catalog::LocationRelease& release : catalog.locationReleases) {
            if (release.graphHash == graphHash && release.nodeHash == nodeHash) {
                ++count;
            }
        }
        return count;
    };

    for (const activity_catalog::Graph& sourceGraph : g_state.catalog.graphs) {
        Node graphNode;
        graphNode.name = hash_label("Activity graph", sourceGraph.hash);
        graphNode.searchText = "activity graph authored positions linked graph browse only";
        graphNode.kind = NodeKind::activityGraph;
        graphNode.status = result.buildMatch ? Status::known : Status::deferred;
        graphNode.producer = Producer::activityCatalog;
        graphNode.provenance = Provenance::catalog;
        graphNode.nativeKey = sourceGraph.hash;
        graphNode.source = source;
        graphNode.actions = Action::copyId;
        graphNode.activityMetadata = ActivityMetadata{};
        graphNode.activityMetadata->graphHash = sourceGraph.hash;
        graphNode.activityMetadata->linkedGraphHashes = sourceGraph.linkedGraphHashes;
        graphNode.activityMetadata->catalogBuild = result.contentBuild;
        graphNode.activityMetadata->catalogVersion = result.version;
        graphNode.activityMetadata->buildMatch = result.buildMatch;
        graphNode.activityMetadata->browseOnly = !result.buildMatch;
        const NodeId graphId = graph.add(std::move(graphNode), result.catalogNode);
        if (!graphId) {
            diagnostics.push_back({Diagnostic::Severity::error,
                                   "The inspection graph reached its node-id capacity for activity graphs."});
            break;
        }

        for (const activity_catalog::GraphNode& sourceNode : sourceGraph.nodes) {
            Node node;
            node.name = hash_label("Graph node", sourceNode.nodeHash);
            node.searchText = "activity graph node authored position state style hash";
            node.kind = NodeKind::activityGraphNode;
            node.status = result.buildMatch ? Status::known : Status::deferred;
            node.producer = Producer::activityCatalog;
            node.provenance = Provenance::catalog;
            node.nativeKey = node_key(sourceGraph.hash, sourceNode.nodeHash);
            node.source = source;
            node.actions = Action::copyId;
            node.activityMetadata = metadata_for(g_state.catalog,
                                                 sourceGraph.hash,
                                                 sourceNode.nodeHash,
                                                 0,
                                                 {sourceNode.authoredX, sourceNode.authoredY},
                                                 static_cast<std::uint32_t>(sourceNode.activityHashes.size()),
                                                 release_count(sourceGraph.hash, sourceNode.nodeHash));
            const NodeId nodeId = graph.add(std::move(node), graphId);
            if (!nodeId) {
                diagnostics.push_back({Diagnostic::Severity::error,
                                       "The inspection graph reached its node-id capacity for activity nodes."});
                break;
            }
            for (const std::uint32_t activityHash : sourceNode.activityHashes) {
                const activity_catalog::Activity* activity =
                    activity_catalog::find_activity(g_state.catalog, activityHash);
                Node reference;
                reference.name = activity != nullptr && !activity->name.empty()
                                     ? activity->name
                                     : hash_label("Activity", activityHash);
                reference.searchText = "activity reference graph node browse only";
                reference.kind = NodeKind::activityReference;
                reference.status = result.buildMatch ? Status::known : Status::deferred;
                reference.producer = Producer::activityCatalog;
                reference.provenance = Provenance::catalog;
                reference.nativeKey =
                    reference_key(sourceGraph.hash, sourceNode.nodeHash, activityHash);
                reference.source = source;
                reference.actions = Action::copyId;
                reference.activityMetadata = metadata_for(
                    g_state.catalog,
                    sourceGraph.hash,
                    sourceNode.nodeHash,
                    activityHash,
                    {sourceNode.authoredX, sourceNode.authoredY},
                    static_cast<std::uint32_t>(sourceNode.activityHashes.size()),
                    release_count(sourceGraph.hash, sourceNode.nodeHash));
                if (!graph.add(std::move(reference), nodeId)) {
                    diagnostics.push_back({Diagnostic::Severity::error,
                                           "The inspection graph reached its node-id capacity for activity references."});
                    break;
                }
            }
        }
    }

    diagnostics.push_back({result.buildMatch ? Diagnostic::Severity::information
                                             : Diagnostic::Severity::warning,
                           result.buildMatch
                               ? "Activity catalog loaded for the target build; authored positions are browse metadata."
                               : "Activity catalog build mismatch; browse only, with no live-session correlation."});
    diagnostics.push_back({Diagnostic::Severity::information,
                           "Authored positions only; no node connections in this catalog."});
    return result;
}

} // namespace sunrise::client::inspection::providers::activity_graph
