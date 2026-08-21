#include "activity_logic_inspection.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <unordered_set>

#include "../../../core/filesystem/path.h"

namespace sunrise::client::inspection::providers::activity_logic {
namespace {

namespace catalog = activity_logic_catalog;
constexpr std::wstring_view kCatalogSuffix = L"\\activity-logic-catalog.bin";
State g_state{};

[[nodiscard]] std::uint64_t group_key(std::uint32_t scenarioTag, catalog::Role role) noexcept {
    return (static_cast<std::uint64_t>(scenarioTag) << 32U)
           | (0x100U + static_cast<std::uint8_t>(role));
}

[[nodiscard]] std::uint64_t placement_key(std::uint32_t definitionTag,
                                          const catalog::Placement& placement,
                                          std::size_t ordinal) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    const auto mix = [&value](std::uint64_t lane) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value ^= static_cast<std::uint8_t>(lane >> shift);
            value *= 1099511628211ULL;
        }
    };
    mix(definitionTag);
    mix(placement.worldId);
    mix(static_cast<std::uint64_t>(ordinal));
    return value == 0 ? 1 : value;
}

[[nodiscard]] std::string definition_label(const catalog::Entity& entity) {
    if (!entity.name.empty()) {
        return entity.name;
    }
    std::array<char, 128> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "%s 0x%08X",
                                      entity.label.empty() ? catalog::role_name(entity.role)
                                                           : entity.label.c_str(),
                                      entity.definitionTag);
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string(catalog::role_name(entity.role));
}

[[nodiscard]] ActivityLogicMetadata metadata_for(const catalog::Entity& entity,
                                                  std::uint32_t scenarioTag) {
    ActivityLogicMetadata metadata{};
    metadata.scenarioTag = scenarioTag;
    metadata.definitionTag = entity.definitionTag;
    metadata.classPrimary = entity.classPrimary;
    metadata.classSecondary = entity.classSecondary;
    metadata.role = static_cast<std::uint8_t>(entity.role);
    metadata.confidence = static_cast<std::uint8_t>(entity.confidence);
    metadata.roleName = catalog::role_name(entity.role);
    metadata.label = entity.label;
    metadata.confidenceName = catalog::confidence_name(entity.confidence);
    metadata.localizedText = entity.localizedText;
    metadata.placementCount = static_cast<std::uint32_t>((std::min)(
        entity.placements.size(),
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
    return metadata;
}

[[nodiscard]] NodeId ensure_group(Graph& graph,
                                  const Source& source,
                                  NodeId root,
                                  std::uint32_t scenarioTag,
                                  catalog::Role role,
                                  std::array<NodeId, 11>& groups) {
    const std::uint8_t index = static_cast<std::uint8_t>(role);
    if (index >= groups.size()) {
        return root;
    }
    if (groups[index]) {
        return groups[index];
    }
    Node group;
    group.name = catalog::role_name(role);
    group.searchText = std::string("authored activity logic group ") + catalog::role_name(role);
    group.kind = NodeKind::logicGroup;
    group.status = Status::known;
    group.producer = Producer::activityLogicCatalog;
    group.provenance = Provenance::catalog;
    group.nativeKey = group_key(scenarioTag, role);
    group.source = source;
    group.actions = Action::copyId;
    groups[index] = graph.add(std::move(group), root);
    return groups[index] ? groups[index] : root;
}

void attach_links(ActivityLogicMetadata& metadata,
                  const catalog::Catalog& source,
                  std::uint32_t entityIndex) {
    const auto append_edge = [&metadata, &source](std::uint32_t edgeIndex, bool outgoing) {
        if (edgeIndex >= source.edges.size()) {
            return;
        }
        const catalog::Edge& edge = source.edges[edgeIndex];
        const std::uint32_t other = outgoing ? edge.targetEntityIndex : edge.sourceEntityIndex;
        if (other >= source.entities.size()) {
            return;
        }
        ActivityLogicRelationship relationship{};
        relationship.definitionTag = source.entities[other].definitionTag;
        relationship.nameHash = edge.nameHash;
        relationship.occurrenceCount = edge.occurrenceCount;
        relationship.outgoing = outgoing;
        metadata.relationships.push_back(relationship);
    };

    for (const std::uint32_t edgeIndex : catalog::outgoing_edges(source, entityIndex)) {
        append_edge(edgeIndex, true);
    }
    for (const std::uint32_t edgeIndex : catalog::incoming_edges(source, entityIndex)) {
        append_edge(edgeIndex, false);
    }
    std::ranges::sort(metadata.relationships, [](const ActivityLogicRelationship& left,
                                                const ActivityLogicRelationship& right) {
        if (left.outgoing != right.outgoing) {
            return left.outgoing;
        }
        if (left.definitionTag != right.definitionTag) {
            return left.definitionTag < right.definitionTag;
        }
        if (left.nameHash != right.nameHash) {
            return left.nameHash < right.nameHash;
        }
        return left.occurrenceCount < right.occurrenceCount;
    });
}

[[nodiscard]] std::string digest_hex(const catalog::Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2U);
    for (const std::uint8_t lane : digest) {
        out.push_back(kHex[lane >> 4U]);
        out.push_back(kHex[lane & 0x0FU]);
    }
    return out;
}

/** Shared hierarchy builder used by both live-match and browse paths. */
void append_activity_nodes(Graph& graph,
                                         std::vector<Diagnostic>& diagnostics,
                                         const catalog::Activity& activity,
                                         const Source& source,
                                         NodeId parent,
                                         AppendResult& result,
                                         bool browseOnly) {
    result.matched = true;
    result.browseOnly = browseOnly;
    result.scenarioTag = activity.scenarioTag;
    result.activityName = activity.name;
    result.destination = activity.destination;
    result.definitionCount = static_cast<std::uint32_t>((std::min)(
        activity.entityIndices.size(),
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));

    Node root;
    const std::string activityLabel = activity.name.empty() ? "Activity logic" : activity.name;
    root.name = browseOnly
                    ? "Browse only — not correlated to current runtime scenario / " + activityLabel
                    : "Activity logic / " + activityLabel;
    root.searchText = "authored static activity encounter logic definitions archive";
    root.kind = NodeKind::activityLogic;
    root.status = Status::known;
    root.producer = Producer::activityLogicCatalog;
    root.provenance = Provenance::catalog;
    root.nativeKey = result.scenarioTag;
    root.source = source;
    root.actions = Action::copyId;
    result.root = graph.add(std::move(root), parent);
    if (!result.root) {
        result.diagnostic = "The inspection graph could not create the activity-logic root.";
        diagnostics.push_back({Diagnostic::Severity::error, result.diagnostic});
        return;
    }

    std::array<NodeId, 11> groups{};
    std::array<std::uint32_t, 11> roleCounts{};
    std::uint32_t strongCount = 0;
    std::uint32_t probableCount = 0;
    std::uint32_t unknownCount = 0;
    std::uint32_t definitionsWithPlacement = 0;
    std::uint32_t definitionsWithRelationships = 0;
    std::uint32_t definitionsWithLocalizedText = 0;
    std::uint32_t emittedDefinitions = 0;
    for (const std::uint32_t entityIndex : activity.entityIndices) {
        if (entityIndex >= g_state.catalog.entities.size()) {
            diagnostics.push_back({Diagnostic::Severity::warning,
                                   "Activity logic catalog contains an out-of-range activity definition reference."});
            continue;
        }
        const catalog::Entity& entity = g_state.catalog.entities[entityIndex];
        const std::uint8_t roleIndex = static_cast<std::uint8_t>(entity.role);
        if (roleIndex < roleCounts.size()) {
            ++roleCounts[roleIndex];
        }
        if (entity.confidence == catalog::Confidence::strong) {
            ++strongCount;
        } else if (entity.confidence == catalog::Confidence::probable) {
            ++probableCount;
        } else {
            ++unknownCount;
        }
        if (!entity.placements.empty()) {
            ++definitionsWithPlacement;
        }
        if (!entity.localizedText.empty()) {
            ++definitionsWithLocalizedText;
        }
        if (!catalog::outgoing_edges(g_state.catalog, entityIndex).empty()
            || !catalog::incoming_edges(g_state.catalog, entityIndex).empty()) {
            ++definitionsWithRelationships;
        }
        const NodeId group = ensure_group(graph,
                                          source,
                                          result.root,
                                          result.scenarioTag,
                                          entity.role,
                                          groups);
        Node node;
        node.name = definition_label(entity);
        node.searchText = std::string("authored activity logic ") + catalog::role_name(entity.role)
                          + " " + entity.label + " " + entity.localizedText;
        node.kind = NodeKind::logicEntity;
        node.status = entity.confidence == catalog::Confidence::strong
                          ? Status::known
                          : Status::unknownSemantic;
        node.producer = Producer::activityLogicCatalog;
        node.provenance = Provenance::catalog;
        node.nativeKey = entity.definitionTag;
        node.source = source;
        node.tag = entity.definitionTag;
        node.classHash = entity.classPrimary;
        node.actions = Action::copyId | Action::copyTag;
        node.activityLogicMetadata = metadata_for(entity, result.scenarioTag);
        attach_links(*node.activityLogicMetadata, g_state.catalog, entityIndex);
        const NodeId entityId = graph.add(std::move(node), group);
        if (!entityId) {
            diagnostics.push_back({Diagnostic::Severity::error,
                                   "The inspection graph reached its node-id capacity while adding activity logic."});
            break;
        }
        ++emittedDefinitions;

        std::size_t ordinal = 0;
        for (const catalog::Placement& placement : entity.placements) {
            Node placementNode;
            std::array<char, 128> label{};
            std::snprintf(label.data(),
                          label.size(),
                          "Authored placement 0x%016llX",
                          static_cast<unsigned long long>(placement.worldId));
            placementNode.name = label.data();
            placementNode.searchText = "authored exact worldid map placement activity logic";
            placementNode.kind = NodeKind::logicPlacement;
            placementNode.status = Status::known;
            placementNode.producer = Producer::activityLogicCatalog;
            placementNode.provenance = Provenance::catalog;
            placementNode.nativeKey = placement_key(entity.definitionTag, placement, ordinal++);
            placementNode.source = source;
            placementNode.tag = entity.definitionTag;
            placementNode.classHash = entity.classPrimary;
            placementNode.worldId = placement.worldId;
            placementNode.transform = Transform{placement.position};
            placementNode.actions = spatial_actions(Action::copyTag);
            placementNode.activityLogicMetadata = metadata_for(entity, result.scenarioTag);
            placementNode.activityLogicMetadata->hasPlacement = true;
            placementNode.activityLogicMetadata->worldId = placement.worldId;
            placementNode.activityLogicMetadata->mapTableTag = placement.mapTableTag;
            placementNode.activityLogicMetadata->placedEntityTag = placement.placedEntityTag;
            placementNode.activityLogicMetadata->authoredRotation = placement.rotation;
            attach_links(*placementNode.activityLogicMetadata, g_state.catalog, entityIndex);
            if (!graph.add(std::move(placementNode), entityId)) {
                diagnostics.push_back({Diagnostic::Severity::error,
                                       "The inspection graph reached its node-id capacity while adding authored logic placements."});
                break;
            }
            ++result.placementCount;
        }
    }

    std::array<char, 320> summary{};
    std::snprintf(summary.data(),
                  summary.size(),
                  "%s scenario 0x%08X (%s): %u definitions, %u exact authored WorldID placements. %s",

                  browseOnly ? "Browsing activity logic" : "Activity logic archive matched",
                  result.scenarioTag,
                  result.activityName.c_str(),
                  result.definitionCount,
                  result.placementCount,
                  browseOnly
                      ? "Browse only — not correlated to current runtime scenario."
                      : "Definitions are static authored evidence, not proof of live enemies, active triggers, or current encounter state.");
    diagnostics.push_back({Diagnostic::Severity::information, summary.data()});

    std::string coverage = "Activity Logic coverage: ";
    coverage += std::to_string(result.definitionCount);
    coverage += " definitions (strong ";
    coverage += std::to_string(strongCount);
    coverage += ", probable ";
    coverage += std::to_string(probableCount);
    coverage += ", unknown ";
    coverage += std::to_string(unknownCount);
    coverage += "), ";
    coverage += std::to_string(definitionsWithPlacement);
    coverage += " definitions with ";
    coverage += std::to_string(result.placementCount);
    coverage += " exact placements, ";
    coverage += std::to_string(definitionsWithRelationships);
    coverage += " with relationships, ";
    coverage += std::to_string(definitionsWithLocalizedText);
    coverage += " with localized text; roles ";
    bool firstRole = true;
    for (std::size_t role = 0; role < roleCounts.size(); ++role) {
        if (roleCounts[role] == 0) {
            continue;
        }
        if (!firstRole) {
            coverage += ", ";
        }
        firstRole = false;
        coverage += catalog::role_name(static_cast<catalog::Role>(role));
        coverage += "=";
        coverage += std::to_string(roleCounts[role]);
    }
    coverage += ".";
    if (emittedDefinitions < result.definitionCount) {
        coverage += " Graph truncated: omitted ";
        coverage += std::to_string(result.definitionCount - emittedDefinitions);
        coverage += " definitions.";
    }
    diagnostics.push_back({Diagnostic::Severity::information, std::move(coverage)});
}

} // namespace

void initialize(void* module) noexcept {
    g_state = {};
    g_state.module = module;
    g_state.initialized = true;
    core::path::Buffer path{};
    if (!core::path::artifact_directory(module, path)
        || !core::path::append(path, kCatalogSuffix)) {
        g_state.load.state = catalog::LoadState::missing;
        g_state.load.diagnostic = "Activity logic catalog artifact path is unavailable.";
        return;
    }
    g_state.load = catalog::load_file(path.chars.data(), g_state.catalog);
}

void shutdown() noexcept {
    g_state = {};
}

bool reload() noexcept {
    if (!g_state.initialized || g_state.module == nullptr) {
        g_state.reloadDiagnostic = "Activity Logic catalog reload is unavailable before initialization.";
        return false;
    }
    core::path::Buffer path{};
    if (!core::path::artifact_directory(g_state.module, path)
        || !core::path::append(path, kCatalogSuffix)) {
        g_state.reloadDiagnostic = "Activity Logic catalog reload path is unavailable.";
        return false;
    }
    catalog::Catalog candidate;
    catalog::LoadResult candidateLoad = catalog::load_file(path.chars.data(), candidate);
    if (candidateLoad.state != catalog::LoadState::ready) {
        g_state.reloadDiagnostic = candidateLoad.diagnostic.empty()
                                       ? "Activity Logic catalog reload rejected the candidate."
                                       : candidateLoad.diagnostic;
        return false;
    }
    g_state.catalog = std::move(candidate);
    g_state.load = std::move(candidateLoad);
    g_state.reloadDiagnostic.clear();
    return true;
}

const State& state() noexcept {
    return g_state;
}

AppendResult append(Graph& graph,
                    std::vector<Diagnostic>& diagnostics,
                    const Source& source,
                    NodeId parent) {
    AppendResult result{};
    result.present = g_state.load.state == catalog::LoadState::ready;
    result.diagnostic = g_state.load.diagnostic;
    if (!result.present) {
        diagnostics.push_back({Diagnostic::Severity::information,
                               result.diagnostic.empty()
                                   ? "No optional activity logic catalog is installed."
                                   : result.diagnostic});
        return result;
    }

    const catalog::Catalog& installed = g_state.catalog;
    std::string provenance = "Activity logic catalog: schema ";
    provenance += std::to_string(catalog::kSchemaVersion);
    provenance += ", converter ";
    provenance += std::to_string(installed.provenance.converterVersion);
    provenance += ", source ";
    provenance += installed.provenance.sourceFormat.empty() ? "unknown"
                                                             : installed.provenance.sourceFormat;
    provenance += ", source SHA-256 ";
    provenance += digest_hex(installed.provenance.sourceDigest);
    provenance += ", ";
    provenance += std::to_string(installed.activities.size());
    provenance += " activities, ";
    provenance += std::to_string(installed.entities.size());
    provenance += " definitions, ";
    std::size_t placementTotal = 0;
    for (const catalog::Entity& entity : installed.entities) {
        placementTotal += entity.placements.size();
    }
    provenance += std::to_string(placementTotal);
    provenance += " authored placements, ";
    provenance += std::to_string(installed.edges.size());
    provenance += " edges. Static research catalog; not live runtime state.";
    diagnostics.push_back({Diagnostic::Severity::information, std::move(provenance)});

    if (!source.scenarioTag.has_value() || *source.scenarioTag == 0) {
        result.diagnostic = "Activity logic catalog is loaded, but the current scenario tag is unavailable.";
        diagnostics.push_back({Diagnostic::Severity::information, result.diagnostic});
        return result;
    }

    result.scenarioTag = *source.scenarioTag;
    const catalog::Activity* activity = catalog::find_activity(g_state.catalog, result.scenarioTag);
    if (activity == nullptr) {
        std::array<char, 128> text{};
        std::snprintf(text.data(),
                      text.size(),
                      "Activity logic catalog has no activity for scenario 0x%08X.",
                      result.scenarioTag);
        result.diagnostic = text.data();
        diagnostics.push_back({Diagnostic::Severity::information, result.diagnostic});
        return result;
    }

    append_activity_nodes(graph, diagnostics, *activity, source, parent, result, false);
    return result;
}

std::vector<BrowseSummary> browse_activities() noexcept {
    std::vector<BrowseSummary> result;
    if (g_state.load.state != catalog::LoadState::ready) {
        return result;
    }
    result.reserve(g_state.catalog.activities.size());
    for (const catalog::Activity& activity : g_state.catalog.activities) {
        result.push_back({activity.scenarioTag, activity.name, activity.destination});
    }
    return result;
}

AppendResult append_browse(Graph& graph,
                           std::vector<Diagnostic>& diagnostics,
                           std::uint32_t scenarioTag,
                           NodeId parent) {
    AppendResult result{};
    result.present = g_state.load.state == catalog::LoadState::ready;
    if (!result.present) {
        result.diagnostic = "No optional activity logic catalog is installed.";
        return result;
    }
    const catalog::Activity* activity = catalog::find_activity(g_state.catalog, scenarioTag);
    if (activity == nullptr) {
        std::array<char, 128> text{};
        std::snprintf(text.data(),
                      text.size(),
                      "Activity logic catalog has no activity for scenario 0x%08X.",
                      scenarioTag);
        result.diagnostic = text.data();
        diagnostics.push_back({Diagnostic::Severity::information, result.diagnostic});
        return result;
    }
    Source source{};
    source.scenarioTag = scenarioTag;
    append_activity_nodes(graph, diagnostics, *activity, source, parent, result, true);
    return result;
}

} // namespace sunrise::client::inspection::providers::activity_logic
