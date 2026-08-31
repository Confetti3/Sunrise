#include "activity_logic_inspection.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_map>

#include "../../../middleware/gameplay/peer/join_messages.h"

namespace sunrise::client::inspection::providers::activity_logic {
namespace {

namespace catalog = activity_logic_catalog;
State g_state{};

[[nodiscard]] std::uint64_t scoped_key(std::uint32_t scenarioTag,
                                       std::uint32_t discriminator) noexcept {
    return (static_cast<std::uint64_t>(scenarioTag) << 32U) | discriminator;
}

[[nodiscard]] std::uint64_t group_key(std::uint32_t scenarioTag, catalog::Role role) noexcept {
    return scoped_key(scenarioTag, 0x100U + static_cast<std::uint8_t>(role));
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

void append_state_var_properties(Node& node, const catalog::StateVar& stateVar) {
    node.properties.push_back({"initial", "Authored initial", static_cast<std::int64_t>(stateVar.initial),
                               Provenance::catalog});
    node.properties.push_back({"lower_clamp", "Authored lower clamp",
                               static_cast<std::int64_t>(stateVar.lowerClamp), Provenance::catalog});
    node.properties.push_back({"upper_clamp", "Authored upper clamp",
                               static_cast<std::int64_t>(stateVar.upperClamp), Provenance::catalog});
    node.properties.push_back({"projection_enabled", "Authored projection enabled",
                               stateVar.projectionEnabled, Provenance::catalog});
    node.properties.push_back({"projection_bytecode_count", "Authored projection bytecode count",
                               static_cast<std::uint64_t>(stateVar.projectionBytecodeCount),
                               Provenance::catalog});
    node.properties.push_back({"projection_constant_count", "Authored projection constant count",
                               static_cast<std::uint64_t>(stateVar.projectionConstantCount),
                               Provenance::catalog});
    node.properties.push_back({"name_proved", "Authored name proved", stateVar.nameProved,
                               Provenance::catalog});
    node.properties.push_back({"trigger_count", "Authored trigger count",
                               static_cast<std::uint64_t>(stateVar.triggers.size()),
                               Provenance::catalog});
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
    metadata.placementCount = static_cast<std::uint32_t>(entity.placements.size());
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
    std::ranges::sort(
        metadata.relationships,
        [](const ActivityLogicRelationship& left, const ActivityLogicRelationship& right) {
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

void append_state_var_nodes(Graph& graph,
                            std::vector<Diagnostic>& diagnostics,
                            const catalog::Catalog& evidence,
                            const Source& source,
                            NodeId parent,
                            std::uint32_t scenarioTag) {
    Node variablesGroup;
    variablesGroup.name = "Variables";
    variablesGroup.searchText = "authored activity logic state variables";
    variablesGroup.kind = NodeKind::logicGroup;
    variablesGroup.status = Status::known;
    variablesGroup.producer = Producer::activityLogicCatalog;
    variablesGroup.provenance = Provenance::catalog;
    variablesGroup.nativeKey = scoped_key(scenarioTag, 0x200U);
    variablesGroup.source = source;
    variablesGroup.actions = Action::copyId;
    const NodeId variablesGroupId = graph.add(std::move(variablesGroup), parent);

    Node ownersGroup;
    ownersGroup.name = "Owners";
    ownersGroup.searchText = "authored activity logic state variable owners";
    ownersGroup.kind = NodeKind::logicGroup;
    ownersGroup.status = Status::known;
    ownersGroup.producer = Producer::activityLogicCatalog;
    ownersGroup.provenance = Provenance::catalog;
    ownersGroup.nativeKey = scoped_key(scenarioTag, 0x201U);
    ownersGroup.source = source;
    ownersGroup.actions = Action::copyId;
    const NodeId ownersGroupId = graph.add(std::move(ownersGroup), parent);

    std::vector<NodeId> stateVarNodes(evidence.stateVars.size());
    for (std::size_t index = 0; index < evidence.stateVars.size(); ++index) {
        const catalog::StateVar& stateVar = evidence.stateVars[index];
        Node variable;
        variable.name = stateVar.name;
        variable.searchText = "authored activity logic state variable " + variable.name;
        variable.kind = NodeKind::logicVariable;
        variable.status = stateVar.nameProved ? Status::known : Status::unknownSemantic;
        variable.producer = Producer::activityLogicCatalog;
        variable.provenance = Provenance::catalog;
        variable.nativeKey = stateVar.configTag;
        variable.stableDiscriminator = scoped_key(scenarioTag, stateVar.configTag);
        variable.source = source;
        variable.tag = stateVar.configTag;
        variable.nameHash = stateVar.nameHash;
        variable.actions = Action::copyId | Action::copyTag;
        append_state_var_properties(variable, stateVar);
        stateVarNodes[index] = graph.add(std::move(variable), variablesGroupId);
        if (!stateVarNodes[index]) {
            diagnostics.push_back({Diagnostic::Severity::error,
                                   "The inspection graph reached its node-id capacity while "
                                   "adding authored state variables."});
            break;
        }
    }

    std::vector<NodeId> rootNodes(evidence.logicRoots.size());
    for (std::size_t index = 0; index < evidence.logicRoots.size(); ++index) {
        const catalog::LogicRoot& logicRoot = evidence.logicRoots[index];
        Node rootNode;
        rootNode.name = logicRoot.name;
        rootNode.searchText = "authored activity logic behavior root " + logicRoot.name;
        rootNode.kind = NodeKind::logicGroup;
        rootNode.status = Status::known;
        rootNode.producer = Producer::activityLogicCatalog;
        rootNode.provenance = Provenance::catalog;
        rootNode.nativeKey = logicRoot.tag;
        rootNode.stableDiscriminator = scoped_key(scenarioTag, logicRoot.tag);
        rootNode.source = source;
        rootNode.tag = logicRoot.tag;
        rootNode.classHash = logicRoot.classId;
        rootNode.actions = Action::copyId | Action::copyTag;
        rootNodes[index] = graph.add(std::move(rootNode), parent);
        if (!rootNodes[index]) {
            diagnostics.push_back({Diagnostic::Severity::error,
                                   "The inspection graph reached its node-id capacity while "
                                   "adding authored behavior roots."});
            break;
        }
    }

    std::unordered_map<std::uint32_t, NodeId> ownerNodes;
    ownerNodes.reserve(evidence.stateVarBindings.size());
    // Materialize class-proven owner identities from the binding table. Keep both directions in
    // the in-memory graph so selecting either endpoint can open the relationship view.
    for (const catalog::StateVarBinding& binding : evidence.stateVarBindings) {
        if (binding.definitionEntityIndex >= evidence.entities.size() || binding.configTag == 0) {
            continue;
        }
        const auto variable = std::ranges::find_if(
            evidence.stateVars,
            [&binding](const catalog::StateVar& stateVar) {
                return stateVar.configTag == binding.configTag;
            });
        if (variable == evidence.stateVars.end()) {
            continue;
        }
        const std::size_t variableIndex =
            static_cast<std::size_t>(variable - evidence.stateVars.begin());
        if (variableIndex >= stateVarNodes.size() || !stateVarNodes[variableIndex]) {
            continue;
        }
        NodeId ownerId{};
        const auto ownerFound = ownerNodes.find(binding.ownerTag);
        if (ownerFound != ownerNodes.end()) {
            ownerId = ownerFound->second;
        } else {
            const catalog::Entity& definition = evidence.entities[binding.definitionEntityIndex];
            Node ownerNode;
            ownerNode.name = definition.name;
            ownerNode.searchText = "authored activity logic owner " + definition.name + " "
                                   + definition.label + " " + definition.localizedText;
            ownerNode.kind = NodeKind::logicEntity;
            ownerNode.status = Status::known;
            ownerNode.producer = Producer::activityLogicCatalog;
            ownerNode.provenance = Provenance::catalog;
            ownerNode.nativeKey = binding.ownerTag;
            ownerNode.stableDiscriminator = scoped_key(scenarioTag, binding.ownerTag);
            ownerNode.source = source;
            ownerNode.tag = binding.ownerTag;
            ownerNode.classHash = 0x80809C0FU;
            ownerNode.actions = Action::copyId | Action::copyTag;
            ownerId = graph.add(std::move(ownerNode), ownersGroupId);
            if (ownerId) {
                ownerNodes.emplace(binding.ownerTag, ownerId);
            }
        }
        Node* ownerNode = graph.node(ownerId);
        Node* variableNode = graph.node(stateVarNodes[variableIndex]);
        if (ownerNode == nullptr || variableNode == nullptr) {
            continue;
        }
        ownerNode->relations.push_back({variableNode->key,
                                        RelationKind::authoredLink,
                                        Provenance::catalog,
                                        1,
                                        true,
                                        0,
                                        -1});
        variableNode->relations.push_back({ownerNode->key,
                                           RelationKind::authoredLink,
                                           Provenance::catalog,
                                           1,
                                           false,
                                           0,
                                           -1});
    }

    for (const catalog::LogicReference& reference : evidence.logicReferences) {
        if (reference.rootIndex >= rootNodes.size()
            || reference.stateVarIndex == catalog::LogicReference::kUnjoinedStateVar
            || reference.stateVarIndex >= stateVarNodes.size()
            || !rootNodes[reference.rootIndex] || !stateVarNodes[reference.stateVarIndex]) {
            continue;
        }
        Node* rootNode = graph.node(rootNodes[reference.rootIndex]);
        Node* variableNode = graph.node(stateVarNodes[reference.stateVarIndex]);
        if (rootNode == nullptr || variableNode == nullptr) {
            continue;
        }
        const RelationKind kind = reference.direction == catalog::LogicReferenceDirection::read
                                      ? RelationKind::logicVariableRead
                                      : RelationKind::logicVariableWrite;
        const bool rootToVariable = reference.direction == catalog::LogicReferenceDirection::write;
        rootNode->relations.push_back({variableNode->key,
                                       kind,
                                       Provenance::catalog,
                                       reference.occurrenceCount,
                                       rootToVariable,
                                       reference.nameHash,
                                       reference.selector});
        variableNode->relations.push_back({rootNode->key,
                                           kind,
                                           Provenance::catalog,
                                           reference.occurrenceCount,
                                           !rootToVariable,
                                           reference.nameHash,
                                           reference.selector});
    }
}

/** Builds the current location's Activity Logic hierarchy. */
void append_activity_nodes(Graph& graph,
                           std::vector<Diagnostic>& diagnostics,
                           const catalog::Catalog& evidence,
                           const catalog::Activity& activity,
                           const Source& source,
                           NodeId parent,
                           AppendResult& result) {
    result.matched = true;
    result.scenarioTag = activity.scenarioTag;
    result.activityName = source.authoredPreview && !source.packageName.empty()
                              ? source.packageName
                              : activity.name;
    result.destination = activity.destination;
    result.definitionCount = static_cast<std::uint32_t>(activity.entityIndices.size());

    Node root;
    root.name = "Activity logic / " + result.activityName;
    root.searchText =
        "authored static activity encounter logic definitions " + result.activityName;
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
    std::unordered_map<std::uint32_t, NodeId> definitionNodes;
    std::vector<NodeId> relationNodes;
    for (const std::uint32_t entityIndex : activity.entityIndices) {
        if (entityIndex >= evidence.entities.size()) {
            diagnostics.push_back(
                {Diagnostic::Severity::warning,
                 "Activity logic catalog contains an out-of-range activity definition reference."});
            continue;
        }
        const catalog::Entity& entity = evidence.entities[entityIndex];
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
        if (!catalog::outgoing_edges(evidence, entityIndex).empty()
            || !catalog::incoming_edges(evidence, entityIndex).empty()) {
            ++definitionsWithRelationships;
        }
        const NodeId group =
            ensure_group(graph, source, result.root, result.scenarioTag, entity.role, groups);
        Node node;
        node.name = entity.name;
        node.searchText = std::string("authored activity logic ") + catalog::role_name(entity.role)
                          + " " + entity.name + " " + entity.label + " "
                          + entity.localizedText;
        node.kind = NodeKind::logicEntity;
        node.status = entity.confidence == catalog::Confidence::strong ? Status::known
                                                                       : Status::unknownSemantic;
        node.producer = Producer::activityLogicCatalog;
        node.provenance = Provenance::catalog;
        node.nativeKey = entity.definitionTag;
        node.source = source;
        node.tag = entity.definitionTag;
        node.classHash = entity.classPrimary;
        node.actions = Action::copyId | Action::copyTag;
        node.activityLogicMetadata = metadata_for(entity, result.scenarioTag);
        attach_links(*node.activityLogicMetadata, evidence, entityIndex);
        const NodeId entityId = graph.add(std::move(node), group);
        if (!entityId) {
            diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph reached its node-id capacity while adding activity logic."});
            break;
        }
        ++emittedDefinitions;
        definitionNodes.emplace(entity.definitionTag, entityId);
        relationNodes.push_back(entityId);

        std::size_t ordinal = 0;
        for (const catalog::Placement& placement : entity.placements) {
            if (!catalog::is_spatial_world_id(placement.worldId)) {
                continue;
            }
            Node placementNode;
            placementNode.name = entity.name;
            placementNode.searchText = "authored exact worldid map placement activity logic "
                                       + entity.name + " " + entity.label;
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
            attach_links(*placementNode.activityLogicMetadata, evidence, entityIndex);
            const NodeId placementId = graph.add(std::move(placementNode), entityId);
            if (!placementId) {
                diagnostics.push_back({Diagnostic::Severity::error,
                                       "The inspection graph reached its node-id capacity while "
                                       "adding authored logic placements."});
                break;
            }
            relationNodes.push_back(placementId);
            ++result.placementCount;
        }
    }

    for (const NodeId nodeId : relationNodes) {
        Node* node = graph.node(nodeId);
        if (node == nullptr || !node->activityLogicMetadata.has_value()) {
            continue;
        }
        for (const ActivityLogicRelationship& relationship :
             node->activityLogicMetadata->relationships) {
            const auto found = definitionNodes.find(relationship.definitionTag);
            const Node* target =
                found == definitionNodes.end() ? nullptr : graph.node(found->second);
            if (target == nullptr) {
                continue;
            }
            node->relations.push_back({target->key,
                                       RelationKind::logic,
                                       Provenance::catalog,
                                       relationship.occurrenceCount,
                                       relationship.outgoing,
                                       relationship.nameHash,
                                       -1});
        }
    }

    append_state_var_nodes(
        graph, diagnostics, evidence, source, result.root, result.scenarioTag);

    std::array<char, 320> summary{};
    std::snprintf(
        summary.data(),
        summary.size(),
        "%s scenario 0x%08X (%s): %u definitions, %u exact authored WorldID placements. %s",

        "Activity logic package evidence",
        result.scenarioTag,
        result.activityName.c_str(),
        result.definitionCount,
        result.placementCount,
        "Definitions are static authored evidence, not proof of live enemies, active triggers, "
        "or current encounter state.");
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
    coverage += " with serialized-name references, ";
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

const State& state() noexcept {
    return g_state;
}

bool activate_location(catalog::Catalog location, Source source) noexcept {
    try {
        std::string error;
        if (!source.scenarioTag.has_value() || *source.scenarioTag == 0
            || (!source.authoredPreview
                && (!source.activitySession.has_value() || *source.activitySession == 0))
            || location.activities.size() != 1
            || location.activities.front().scenarioTag != *source.scenarioTag
            || location.provenance.contentBuild != middleware::gameplay::peer::kHostBuild
            || !catalog::validate(location, error)) {
            return false;
        }
        g_state.locationCatalog = std::move(location);
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
    const bool previewMatch = g_state.activationSource.authoredPreview;
    const bool liveMatch =
        !previewMatch && !source.authoredPreview && source.scenarioTag.has_value()
        && *source.scenarioTag == g_state.locationScenarioTag
        && g_state.activationSource.activitySession.has_value()
        && source.activitySession.has_value()
        && *g_state.activationSource.activitySession == *source.activitySession;
    const bool locationMatch = g_state.locationActive && (previewMatch || liveMatch);
    result.present = locationMatch;
    result.diagnostic = locationMatch
                            ? (g_state.activationSource.authoredPreview
                                   ? "Authored Activity Logic preview loaded."
                                   : "Current-location Activity Logic cache loaded.")
                                      : "No matching current-location Activity Logic is active.";
    if (!result.present) {
        return result;
    }

    const catalog::Catalog& evidence = g_state.locationCatalog;
    const Source& evidenceSource =
        g_state.activationSource.authoredPreview ? g_state.activationSource : source;
    std::string provenance = "Activity Logic package cache: schema ";
    provenance += std::to_string(catalog::kSchemaVersion);
    provenance += ", collector ";
    provenance += std::to_string(evidence.provenance.collectorVersion);
    provenance += ", content SHA-256 ";
    provenance += digest_hex(evidence.provenance.contentFingerprint);
    provenance += ", ";
    provenance += std::to_string(evidence.activities.size());
    provenance += " activities, ";
    provenance += std::to_string(evidence.entities.size());
    provenance += " definitions, ";
    std::size_t placementTotal = 0;
    for (const catalog::Entity& entity : evidence.entities) {
        placementTotal += entity.placements.size();
    }
    provenance += std::to_string(placementTotal);
    provenance += " authored placements, ";
    provenance += std::to_string(evidence.edges.size());
    provenance += " serialized-name references, ";
    provenance += std::to_string(evidence.stateVars.size());
    provenance += " StateVars, ";
    provenance += std::to_string(evidence.stateVarBindings.size());
    provenance += " owner bindings, ";
    provenance += std::to_string(evidence.logicRoots.size());
    provenance += " behavior roots, ";
    provenance += std::to_string(evidence.logicReferences.size());
    provenance += " variable references. Static authored data; not execution flow or live runtime state.";
    diagnostics.push_back({Diagnostic::Severity::information, std::move(provenance)});

    if (!evidenceSource.scenarioTag.has_value() || *evidenceSource.scenarioTag == 0) {
        result.diagnostic =
            "Activity logic catalog is loaded, but the current scenario tag is unavailable.";
        diagnostics.push_back({Diagnostic::Severity::information, result.diagnostic});
        return result;
    }

    result.scenarioTag = *evidenceSource.scenarioTag;
    const catalog::Activity* activity = catalog::find_activity(evidence, result.scenarioTag);
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

    append_activity_nodes(graph, diagnostics, evidence, *activity, evidenceSource, parent, result);
    return result;
}

} // namespace sunrise::client::inspection::providers::activity_logic
