#include "activity_graph_packages.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../middleware/content/packages/tables/internal.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/gameplay/peer/join_messages.h"
#include "../../../state/content/content_catalog.h"
#include "../../../state/build_data/scenarios/scenario_catalog.h"

namespace sunrise::client::content::activity::graph_packages {
namespace {

namespace catalog = inspection::activity_catalog;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

constexpr std::uint32_t kClientIndexTag = 0x81327D62U;
constexpr std::uint32_t kPageIndexTag = 0x81327D04U;
constexpr std::uint32_t kActivityIndexTag = 0x81327CF0U;
constexpr std::uint32_t kClientIndexClass = 0x80805E73U;
constexpr std::uint32_t kPageIndexClass = 0x808075CAU;
constexpr std::uint32_t kActivityIndexClass = 0x808076F0U;
constexpr std::uint32_t kClientIndexRowClass = 0x80805E77U;
constexpr std::uint32_t kPageIndexRowClass = 0x80807510U;
constexpr std::uint32_t kActivityIndexRowClass = 0x808076FCU;
constexpr std::uint32_t kClientGraphClass = 0x80805E79U;
constexpr std::uint32_t kPageClass = 0x80807512U;
constexpr std::uint32_t kGraphNodeClass = 0x80805E88U;
constexpr std::uint32_t kPageNodeClass = 0x80807514U;
constexpr std::uint32_t kPageActivityClass = 0x80807519U;
constexpr std::uint32_t kPageStateClass = 0x80807517U;
constexpr std::uint32_t kLinkedGraphClass = 0x80805E84U;
constexpr std::uint32_t kLinkedGraphEntryClass = 0x808076D8U;
constexpr std::size_t kIndexDescriptor = 8;
constexpr std::size_t kIndexRowStride = 24;
constexpr std::size_t kIndexTargetTag = 16;
constexpr std::size_t kActivityIndexRowStride = 16;
constexpr std::size_t kActivityRecordRelative = 8;
constexpr std::size_t kActivityNameRelative = 0x68;
constexpr std::size_t kGraphLinksDescriptor = 0x40;
constexpr std::size_t kGraphNodesDescriptor = 0x50;
constexpr std::size_t kGraphNodeStride = 80;
constexpr std::size_t kGraphNodePosition = 0x28;
constexpr std::size_t kPageNodesDescriptor = 8;
constexpr std::size_t kPageNodeStride = 40;
constexpr std::size_t kPageNodeActivitiesDescriptor = 8;
constexpr std::size_t kPageNodeStatesDescriptor = 0x18;
constexpr std::size_t kPageActivityStride = 24;
constexpr std::size_t kPageActivityIndex = 4;
constexpr std::size_t kPageStateStride = 24;
constexpr std::size_t kLinkedGraphStride = 64;
constexpr std::size_t kLinkedGraphEntriesDescriptor = 8;
constexpr std::size_t kLinkedGraphEntryStride = 24;
constexpr std::size_t kGraphCount = 26;
constexpr std::size_t kActivityCount = 1170;
constexpr std::size_t kMaximumNodes = 4096;
constexpr std::size_t kMaximumActivityReferences = 8192;
constexpr std::string_view kScenarioSuffix = ":scenario_client";

struct IndexRow final {
    std::uint32_t hash{};
    std::uint32_t clientTag{};
    std::uint32_t pageTag{};
};

struct ActivityDefinition final {
    std::uint32_t hash{};
    std::string name;
};

struct Build final {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    Cancelled cancelled{};
    void* cancelContext{};
    Progress* progress{};
};

[[nodiscard]] bool stopped(const Build& build) noexcept {
    return build.cancelled != nullptr && build.cancelled(build.cancelContext);
}

[[nodiscard]] bool fail(Build& build, std::string message) {
    ++build.progress->rejected;
    build.progress->diagnostic = std::move(message);
    return false;
}

[[nodiscard]] bool cancel(Build& build) {
    build.progress->diagnostic = "Activity Graph package collection cancelled.";
    return false;
}

[[nodiscard]] bool checked_relative(std::size_t field,
                                    std::int64_t relative,
                                    std::size_t size,
                                    std::size_t& target) noexcept {
    constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
    constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
    if (field > static_cast<std::size_t>(kMax)) {
        return false;
    }
    const std::int64_t base = static_cast<std::int64_t>(field);
    if ((relative > 0 && base > kMax - relative)
        || (relative < 0 && base < kMin - relative)) {
        return false;
    }
    const std::int64_t result = base + relative;
    if (result < 0 || static_cast<std::uint64_t>(result) > size) {
        return false;
    }
    target = static_cast<std::size_t>(result);
    return true;
}

[[nodiscard]] bool array_at(std::span<const std::byte> blob,
                            std::size_t descriptor,
                            std::uint32_t elementClass,
                            std::size_t stride,
                            std::uint64_t maximum,
                            tables::Array& output) noexcept {
    output = {};
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    if (!tables::read(blob, descriptor, count)
        || !tables::read(blob, descriptor + sizeof count, relative)) {
        return false;
    }
    if (count == 0) {
        return relative == 0;
    }
    if (count > maximum || !tables::find_array_at(blob, descriptor, output)
        || output.elementClass != elementClass || output.dataOffset > blob.size()
        || output.count > (blob.size() - output.dataOffset) / stride) {
        output = {};
        return false;
    }
    return true;
}

[[nodiscard]] bool read_expected(Build& build,
                                 std::uint32_t tag,
                                 std::uint32_t expectedClass,
                                 std::vector<std::byte>& bytes) noexcept {
    bytes.clear();
    std::uint32_t classId = 0;
    return !stopped(build)
           && reader::read_tag(*build.source, *build.scratch, tag, bytes, classId)
           && classId == expectedClass;
}

[[nodiscard]] bool scenario_stem(std::uint32_t scenarioTag, std::string& stem) noexcept {
    stem.clear();
    std::vector<state::content::Definition> definitions(
        state::content::kDefinitionCatalogCapacity);
    std::size_t count = 0;
    if (!state::content::snapshot(definitions, count)) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const state::content::Definition& definition = definitions[index];
        if (definition.tag != scenarioTag || definition.classId != tables::kScenarioClass) {
            continue;
        }
        const std::string_view name(definition.name.data(), definition.nameLength);
        if (!name.ends_with(kScenarioSuffix) || name.size() == kScenarioSuffix.size()) {
            continue;
        }
        const std::string_view candidate = name.substr(0, name.size() - kScenarioSuffix.size());
        if (!stem.empty() && stem != candidate) {
            return false;
        }
        stem.assign(candidate);
    }
    return !stem.empty();
}

[[nodiscard]] bool index_rows(Build& build,
                              std::span<const std::byte> client,
                              std::span<const std::byte> pages,
                              std::array<IndexRow, kGraphCount>& rows) {
    tables::Array clientRows{};
    tables::Array pageRows{};
    if (!array_at(client,
                  kIndexDescriptor,
                  kClientIndexRowClass,
                  kIndexRowStride,
                  kGraphCount,
                  clientRows)
        || !array_at(pages,
                     kIndexDescriptor,
                     kPageIndexRowClass,
                     kIndexRowStride,
                     kGraphCount,
                     pageRows)
        || clientRows.count != kGraphCount || pageRows.count != kGraphCount) {
        return fail(build, "Activity Graph indexes do not have the build-86657 shape.");
    }
    std::unordered_set<std::uint32_t> hashes;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::size_t clientOffset = clientRows.dataOffset + index * kIndexRowStride;
        const std::size_t pageOffset = pageRows.dataOffset + index * kIndexRowStride;
        std::uint32_t pageHash = 0;
        if (!tables::read(client, clientOffset, rows[index].hash)
            || !tables::read(client, clientOffset + kIndexTargetTag, rows[index].clientTag)
            || !tables::read(pages, pageOffset, pageHash)
            || !tables::read(pages, pageOffset + kIndexTargetTag, rows[index].pageTag)
            || rows[index].hash == 0 || pageHash != rows[index].hash
            || tables::package_of(rows[index].clientTag) == tables::kAbsentPackageId
            || tables::package_of(rows[index].pageTag) == tables::kAbsentPackageId) {
            return fail(build, "Activity Graph index rows disagree or contain an invalid tag.");
        }
        if (!hashes.insert(rows[index].hash).second) {
            return fail(build, "Activity Graph index contains a duplicate graph hash.");
        }
    }
    return true;
}

[[nodiscard]] bool activity_definitions(Build& build,
                                        std::span<const std::byte> blob,
                                        std::vector<ActivityDefinition>& definitions) {
    tables::Array rows{};
    if (!array_at(blob,
                  kIndexDescriptor,
                  kActivityIndexRowClass,
                  kActivityIndexRowStride,
                  kActivityCount,
                  rows)
        || rows.count != kActivityCount) {
        return fail(build, "Activity index does not have the build-86657 shape.");
    }
    definitions.reserve(kActivityCount);
    std::unordered_set<std::uint32_t> hashes;
    for (std::size_t index = 0; index < kActivityCount; ++index) {
        if (stopped(build)) {
            return cancel(build);
        }
        const std::size_t row = rows.dataOffset + index * kActivityIndexRowStride;
        ActivityDefinition definition{};
        std::int64_t recordRelative = 0;
        std::size_t record = 0;
        if (!tables::read(blob, row, definition.hash)
            || !tables::read(blob, row + kActivityRecordRelative, recordRelative)
            || definition.hash == 0
            || !checked_relative(row + kActivityRecordRelative, recordRelative, blob.size(), record)) {
            return fail(build, "Activity index contains an invalid record pointer.");
        }
        if (!hashes.insert(definition.hash).second) {
            return fail(build, "Activity index contains a duplicate activity hash.");
        }
        std::uint32_t repeatedHash = 0;
        std::int64_t nameRelative = 0;
        const std::size_t nameField = record + kActivityNameRelative;
        std::size_t nameOffset = 0;
        if (!tables::read(blob, record, repeatedHash) || repeatedHash != definition.hash
            || !tables::read(blob, nameField, nameRelative)) {
            return fail(build, "Activity record identity is invalid.");
        }
        if (nameRelative != 0) {
            if (!checked_relative(nameField, nameRelative, blob.size(), nameOffset)) {
                return fail(build, "Activity record name pointer is invalid.");
            }
            std::size_t end = nameOffset;
            while (end < blob.size() && blob[end] != std::byte{0}
                   && end - nameOffset < state::content::kDefinitionNameCapacity) {
                const unsigned char value = static_cast<unsigned char>(blob[end]);
                if (value < 0x20 || value > 0x7E) {
                    return fail(build, "Activity record name is not printable ASCII.");
                }
                ++end;
            }
            if (end == blob.size() || blob[end] != std::byte{0}
                || end - nameOffset >= state::content::kDefinitionNameCapacity) {
                return fail(build, "Activity record name is not bounded.");
            }
            definition.name.assign(reinterpret_cast<const char*>(blob.data() + nameOffset),
                                   end - nameOffset);
        }
        definitions.push_back(std::move(definition));
    }
    return true;
}

[[nodiscard]] bool graph_nodes(Build& build,
                               const IndexRow& index,
                               std::span<const ActivityDefinition> activities,
                               catalog::Graph& graph) {
    std::vector<std::byte> clientBytes;
    std::vector<std::byte> pageBytes;
    if (!read_expected(build, index.clientTag, kClientGraphClass, clientBytes)
        || !read_expected(build, index.pageTag, kPageClass, pageBytes)) {
        if (stopped(build)) {
            return cancel(build);
        }
        return fail(build, "An indexed Activity Graph blob could not be read.");
    }
    tables::Array clientNodes{};
    tables::Array pageNodes{};
    if (!array_at(clientBytes,
                  kGraphNodesDescriptor,
                  kGraphNodeClass,
                  kGraphNodeStride,
                  kMaximumNodes,
                  clientNodes)
        || !array_at(pageBytes,
                     kPageNodesDescriptor,
                     kPageNodeClass,
                     kPageNodeStride,
                     kMaximumNodes,
                     pageNodes)
        || clientNodes.count != pageNodes.count) {
        return fail(build, "Activity Graph client/page node arrays disagree.");
    }
    graph.hash = index.hash;
    graph.nodes.reserve(static_cast<std::size_t>(clientNodes.count));
    std::unordered_set<std::uint32_t> nodeIds;
    std::size_t activityReferences = 0;
    for (std::uint64_t ordinal = 0; ordinal < clientNodes.count; ++ordinal) {
        if (stopped(build)) {
            return cancel(build);
        }
        const std::size_t clientNode = clientNodes.dataOffset
                                       + static_cast<std::size_t>(ordinal) * kGraphNodeStride;
        const std::size_t pageNode =
            pageNodes.dataOffset + static_cast<std::size_t>(ordinal) * kPageNodeStride;
        catalog::GraphNode node{};
        std::int16_t authoredX = 0;
        std::int16_t authoredY = 0;
        if (!tables::read(pageBytes, pageNode, node.nodeHash)
            || !tables::read(clientBytes, clientNode + kGraphNodePosition, authoredX)
            || !tables::read(clientBytes,
                             clientNode + kGraphNodePosition + sizeof authoredX,
                             authoredY)
            || node.nodeHash == 0 || !nodeIds.insert(node.nodeHash).second) {
            return fail(build, "Activity Graph contains an invalid or duplicate node id.");
        }
        node.graphHash = graph.hash;
        node.authoredX = static_cast<float>(authoredX);
        node.authoredY = static_cast<float>(authoredY);
        tables::Array states{};
        if (!array_at(pageBytes,
                      pageNode + kPageNodeStatesDescriptor,
                      kPageStateClass,
                      kPageStateStride,
                      kMaximumActivityReferences,
                      states)) {
            return fail(build, "Activity Graph node state array is invalid.");
        }
        node.stateValues.reserve(static_cast<std::size_t>(states.count));
        for (std::uint64_t state = 0; state < states.count; ++state) {
            const std::size_t offset =
                states.dataOffset + static_cast<std::size_t>(state) * kPageStateStride;
            std::uint32_t value = 0;
            if (!tables::read(pageBytes, offset, value) || value > 4U) {
                return fail(build, "Activity Graph node has an invalid native state value.");
            }
            node.stateValues.push_back(value);
        }
        tables::Array references{};
        if (!array_at(pageBytes,
                      pageNode + kPageNodeActivitiesDescriptor,
                      kPageActivityClass,
                      kPageActivityStride,
                      kMaximumActivityReferences,
                      references)) {
            return fail(build, "Activity Graph node activity array is invalid.");
        }
        activityReferences += static_cast<std::size_t>(references.count);
        if (activityReferences > kMaximumActivityReferences) {
            return fail(build, "Activity Graph activity reference count is excessive.");
        }
        for (std::uint64_t reference = 0; reference < references.count; ++reference) {
            const std::size_t offset = references.dataOffset
                                       + static_cast<std::size_t>(reference)
                                             * kPageActivityStride;
            std::uint32_t owner = 0;
            std::uint32_t activityIndex = 0;
            if (!tables::read(pageBytes, offset, owner)
                || !tables::read(pageBytes, offset + kPageActivityIndex, activityIndex)
                || owner != node.nodeHash || activityIndex >= activities.size()) {
                return fail(build, "Activity Graph node has an invalid activity reference.");
            }
            const std::uint32_t hash = activities[activityIndex].hash;
            if (std::ranges::find(node.activityHashes, hash) == node.activityHashes.end()) {
                node.activityHashes.push_back(hash);
            }
        }
        graph.nodes.push_back(std::move(node));
    }

    tables::Array links{};
    if (!array_at(clientBytes,
                  kGraphLinksDescriptor,
                  kLinkedGraphClass,
                  kLinkedGraphStride,
                  kGraphCount,
                  links)) {
        return fail(build, "Activity Graph link array is invalid.");
    }
    // Link indices are resolved after all 26 graph hashes have been read.
    graph.linkedGraphHashes.reserve(static_cast<std::size_t>(links.count));
    for (std::uint64_t row = 0; row < links.count; ++row) {
        const std::size_t link =
            links.dataOffset + static_cast<std::size_t>(row) * kLinkedGraphStride;
        tables::Array entries{};
        if (!array_at(clientBytes,
                      link + kLinkedGraphEntriesDescriptor,
                      kLinkedGraphEntryClass,
                      kLinkedGraphEntryStride,
                      kGraphCount,
                      entries)) {
            return fail(build, "Activity Graph linked-entry array is invalid.");
        }
        // Temporarily retain graph ordinals; build() replaces them with definition hashes.
        for (std::uint64_t entry = 0; entry < entries.count; ++entry) {
            const std::size_t offset = entries.dataOffset
                                       + static_cast<std::size_t>(entry)
                                             * kLinkedGraphEntryStride;
            std::uint8_t graphIndex = 0;
            if (!tables::read(clientBytes, offset, graphIndex)) {
                return fail(build, "Activity Graph linked-entry is truncated.");
            }
            if (graphIndex == 0xFFU) {
                continue;
            }
            if (graphIndex >= kGraphCount) {
                return fail(build, "Activity Graph linked-entry index is invalid.");
            }
            const std::uint32_t ordinalValue = graphIndex;
            if (std::ranges::find(graph.linkedGraphHashes, ordinalValue)
                == graph.linkedGraphHashes.end()) {
                graph.linkedGraphHashes.push_back(ordinalValue);
            }
        }
    }
    return true;
}

} // namespace

bool build(const reader::Source& source,
           reader::Scratch& scratch,
           std::uint32_t scenarioTag,
           std::string_view mapFamily,
           std::span<const std::byte> contentFingerprint,
           Cancelled cancelled,
           void* cancelContext,
           catalog::Catalog& output,
           Progress& progress) noexcept {
    output = {};
    progress = {};
    try {
        Build build{&source, &scratch, cancelled, cancelContext, &progress};
        if (scenarioTag == 0 || mapFamily.empty()
            || contentFingerprint.size() != catalog::kDigestSize) {
            return fail(build, "Activity Graph collection scope is incomplete.");
        }
        std::string stem;
        if (!scenario_stem(scenarioTag, stem)) {
            return fail(build, "Current scenario has no exact named-tag association.");
        }

        std::vector<std::byte> clientIndexBytes;
        std::vector<std::byte> pageIndexBytes;
        std::vector<std::byte> activityIndexBytes;
        if (!read_expected(build, kClientIndexTag, kClientIndexClass, clientIndexBytes)
            || !read_expected(build, kPageIndexTag, kPageIndexClass, pageIndexBytes)
            || !read_expected(build, kActivityIndexTag, kActivityIndexClass, activityIndexBytes)) {
            if (stopped(build)) {
                return cancel(build);
            }
            return fail(build, "Build-86657 Activity Graph roots could not be read.");
        }
        std::array<IndexRow, kGraphCount> indexes{};
        if (!index_rows(build, clientIndexBytes, pageIndexBytes, indexes)) {
            return false;
        }
        std::vector<ActivityDefinition> definitions;
        if (!activity_definitions(build, activityIndexBytes, definitions)) {
            return false;
        }
        std::unordered_set<std::uint32_t> scenarioActivities;
        for (const ActivityDefinition& definition : definitions) {
            if (definition.name == stem) {
                scenarioActivities.insert(definition.hash);
            }
            state::build_data::scenarios::Definition scenario{};
            if (definition.name.empty()
                || !state::build_data::scenarios::find(definition.name, scenario)
                || scenario.spawnStemLength == 0
                || scenario.spawnStemLength > scenario.spawnStem.size()) {
                continue;
            }
            const std::string_view spawnStem(scenario.spawnStem.data(),
                                             scenario.spawnStemLength);
            if (spawnStem == mapFamily) {
                scenarioActivities.insert(definition.hash);
            }
        }
        if (scenarioActivities.empty()) {
            return fail(build,
                        "Current scenario and map family have no build-86657 activity definition.");
        }

        std::array<catalog::Graph, kGraphCount> candidates{};
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (!graph_nodes(build, indexes[index], definitions, candidates[index])) {
                return false;
            }
        }
        for (catalog::Graph& graph : candidates) {
            for (std::uint32_t& ordinal : graph.linkedGraphHashes) {
                ordinal = indexes[ordinal].hash;
            }
        }

        std::array<bool, kGraphCount> selected{};
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            for (const catalog::GraphNode& node : candidates[index].nodes) {
                if (std::ranges::any_of(node.activityHashes, [&](std::uint32_t hash) {
                        return scenarioActivities.contains(hash);
                    })) {
                    selected[index] = true;
                    break;
                }
            }
        }
        if (std::ranges::none_of(selected, [](bool value) { return value; })) {
            return fail(build, "Current scenario has no Activity Graph node.");
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t index = 0; index < selected.size(); ++index) {
                if (!selected[index]) {
                    continue;
                }
                for (const std::uint32_t linked : candidates[index].linkedGraphHashes) {
                    const auto target = std::ranges::find(indexes, linked, &IndexRow::hash);
                    if (target == indexes.end()) {
                        return fail(build, "Activity Graph link leaves the verified index.");
                    }
                    const std::size_t targetIndex =
                        static_cast<std::size_t>(target - indexes.begin());
                    if (!selected[targetIndex]) {
                        selected[targetIndex] = true;
                        changed = true;
                    }
                }
            }
            if (stopped(build)) {
                return cancel(build);
            }
        }

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> activityGraphs;
        for (std::size_t index = 0; index < selected.size(); ++index) {
            if (!selected[index]) {
                continue;
            }
            progress.nodes += candidates[index].nodes.size();
            progress.links += candidates[index].linkedGraphHashes.size();
            for (const catalog::GraphNode& node : candidates[index].nodes) {
                progress.states += node.stateValues.size();
                for (const std::uint32_t activityHash : node.activityHashes) {
                    auto& graphs = activityGraphs[activityHash];
                    if (std::ranges::find(graphs, candidates[index].hash) == graphs.end()) {
                        graphs.push_back(candidates[index].hash);
                    }
                }
            }
            output.graphs.push_back(std::move(candidates[index]));
        }
        std::unordered_map<std::uint32_t, const ActivityDefinition*> definitionsByHash;
        definitionsByHash.reserve(definitions.size());
        for (const ActivityDefinition& definition : definitions) {
            definitionsByHash.emplace(definition.hash, &definition);
        }
        std::vector<std::uint32_t> activityHashes;
        activityHashes.reserve(activityGraphs.size());
        for (const auto& [hash, graphs] : activityGraphs) {
            activityHashes.push_back(hash);
        }
        std::ranges::sort(activityHashes);
        for (const std::uint32_t hash : activityHashes) {
            const auto definition = definitionsByHash.find(hash);
            if (definition == definitionsByHash.end()) {
                return fail(build, "Activity Graph references an unknown activity hash.");
            }
            catalog::Activity activity{};
            activity.hash = hash;
            activity.name = definition->second->name;
            activity.graphHashes = std::move(activityGraphs[hash]);
            output.activities.push_back(std::move(activity));
        }

        output.contentBuild = middleware::gameplay::peer::kHostBuild;
        output.collectorVersion = catalog::kCollectorVersion;
        output.scenarioTag = scenarioTag;
        std::transform(contentFingerprint.begin(),
                       contentFingerprint.end(),
                       output.contentFingerprint.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
        progress.activities = output.activities.size();
        progress.graphs = output.graphs.size();
        std::string error;
        if (!catalog::validate(output, error)) {
            output = {};
            return fail(build, "Activity Graph catalogue validation failed: " + error);
        }
        if (stopped(build)) {
            output = {};
            return cancel(build);
        }
        progress.diagnostic = "Activity Graph package catalogue built.";
        return true;
    } catch (...) {
        output = {};
        progress.diagnostic = "Activity Graph package collection failed.";
        return false;
    }
}

} // namespace sunrise::client::content::activity::graph_packages
