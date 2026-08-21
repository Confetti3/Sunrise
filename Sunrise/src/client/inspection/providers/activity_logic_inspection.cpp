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
    std::unordered_set<std::uint32_t> linked;
    for (const catalog::Edge& edge : source.edges) {
        std::uint32_t other = (std::numeric_limits<std::uint32_t>::max)();
        if (edge.sourceEntityIndex == entityIndex) {
            other = edge.targetEntityIndex;
        } else if (edge.targetEntityIndex == entityIndex) {
            other = edge.sourceEntityIndex;
        }
        if (other < source.entities.size()) {
            linked.insert(source.entities[other].definitionTag);
        }
    }
    metadata.linkedDefinitionTags.assign(linked.begin(), linked.end());
    std::ranges::sort(metadata.linkedDefinitionTags);
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

} // namespace

void initialize(void* module) noexcept {
    g_state = {};
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

    result.matched = true;
    result.activityName = activity->name;
    result.destination = activity->destination;
    result.definitionCount = static_cast<std::uint32_t>((std::min)(
        activity->entityIndices.size(),
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));

    Node root;
    root.name = activity->name.empty() ? "Activity logic" : "Activity logic / " + activity->name;
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
        return result;
    }

    std::array<NodeId, 11> groups{};
    for (const std::uint32_t entityIndex : activity->entityIndices) {
        if (entityIndex >= g_state.catalog.entities.size()) {
            diagnostics.push_back({Diagnostic::Severity::warning,
                                   "Activity logic catalog contains an out-of-range activity definition reference."});
            continue;
        }
        const catalog::Entity& entity = g_state.catalog.entities[entityIndex];
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
                  "Activity logic archive matched scenario 0x%08X (%s): %u definitions, %u exact authored WorldID placements. Definitions are static authored evidence, not proof of live enemies, active triggers, or current encounter state.",
                  result.scenarioTag,
                  result.activityName.c_str(),
                  result.definitionCount,
                  result.placementCount);
    diagnostics.push_back({Diagnostic::Severity::information, summary.data()});
    return result;
}

} // namespace sunrise::client::inspection::providers::activity_logic
