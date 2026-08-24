#include "activity_graph_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace sunrise::client::inspection::activity_catalog {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'S', 'A', 'C', 'A', 'T', '0', '0', '1'};
constexpr std::size_t kSectionCount = 6;
constexpr std::array<std::uint32_t, kSectionCount> kExpectedStrides{20, 20, 40, 4, 4, 40};
constexpr std::size_t kActivitySection = 0;
constexpr std::size_t kGraphSection = 1;
constexpr std::size_t kNodeSection = 2;
constexpr std::size_t kActivityRefSection = 3;
constexpr std::size_t kLinkedRefSection = 4;
constexpr std::size_t kLocationSection = 5;
constexpr std::size_t kMaximumFileSize = 256U * 1024U * 1024U;
constexpr std::uint32_t kMaximumActivities = 100000;
constexpr std::uint32_t kMaximumGraphs = 10000;
constexpr std::uint32_t kMaximumNodes = 250000;
constexpr std::uint32_t kMaximumReferences = 2000000;
constexpr std::uint32_t kMaximumLocations = 500000;

struct Section final {
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t stride{};
};

struct Reader final {
    std::span<const std::byte> bytes;

    [[nodiscard]] bool read_u8(std::size_t offset, std::uint8_t& value) const noexcept {
        if (offset >= bytes.size()) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(bytes[offset]);
        return true;
    }

    [[nodiscard]] bool read_u32(std::size_t offset, std::uint32_t& value) const noexcept {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
            return false;
        }
        value =
            static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset]))
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16U)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24U);
        return true;
    }

    [[nodiscard]] bool read_f32(std::size_t offset, float& value) const noexcept {
        std::uint32_t bits{};
        if (!read_u32(offset, bits)) {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    [[nodiscard]] bool read_bytes(std::size_t offset,
                                  std::span<std::uint8_t> output) const noexcept {
        if (offset > bytes.size() || output.size() > bytes.size() - offset) {
            return false;
        }
        for (std::size_t index = 0; index < output.size(); ++index) {
            output[index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
        }
        return true;
    }
};

[[nodiscard]] bool safe_range(std::size_t total,
                              std::uint32_t offset,
                              std::uint32_t count,
                              std::uint32_t stride,
                              std::uint32_t maximum) noexcept {
    if (count > maximum || stride == 0 || offset > total) {
        return false;
    }
    return static_cast<std::size_t>(count) <= (total - offset) / stride;
}

[[nodiscard]] bool valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const std::uint8_t first = std::to_integer<std::uint8_t>(bytes[index]);
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first <= 0x7FU) {
            length = 1;
            codepoint = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (length > bytes.size() - index) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[index + continuation]);
            if ((value & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (value & 0x3FU);
        }
        if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U)
            || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] bool string_at(const Reader& reader,
                             const Section& strings,
                             std::uint32_t offset,
                             std::uint32_t length,
                             std::string& output) {
    const std::size_t begin = strings.offset;
    const std::size_t end = begin + strings.count;
    if (offset < begin || offset > end || length > end - offset) {
        return false;
    }
    const std::span<const std::byte> bytes = reader.bytes.subspan(offset, length);
    for (const std::byte value : bytes) {
        if (std::to_integer<std::uint8_t>(value) == 0) {
            return false;
        }
    }
    if (!valid_utf8(bytes)) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

[[nodiscard]] bool fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

[[nodiscard]] bool graph_exists(const std::unordered_set<std::uint32_t>& hashes,
                                std::uint32_t hash) noexcept {
    return hashes.contains(hash);
}

} // namespace

bool validate(const Catalog& catalog, std::string& error) {
    error.clear();
    if (catalog.contentBuild == 0 || catalog.manifestVersion.empty()) {
        return fail(error, "catalog identity is incomplete");
    }
    if (catalog.activities.size() > kMaximumActivities || catalog.graphs.size() > kMaximumGraphs
        || catalog.locationReleases.size() > kMaximumLocations) {
        return fail(error, "catalog count exceeds the supported maximum");
    }

    std::unordered_set<std::uint32_t> activityHashes;
    std::unordered_set<std::uint32_t> graphHashes;
    activityHashes.reserve(catalog.activities.size());
    graphHashes.reserve(catalog.graphs.size());
    for (const Activity& activity : catalog.activities) {
        if (activity.hash == 0 || !activityHashes.insert(activity.hash).second) {
            return fail(error, "duplicate or zero activity hash");
        }
    }
    for (const Graph& graph : catalog.graphs) {
        if (graph.hash == 0 || !graphHashes.insert(graph.hash).second) {
            return fail(error, "duplicate or zero graph hash");
        }
    }

    std::unordered_set<std::uint64_t> nodeKeys;
    std::size_t referenceCount = 0;
    for (const Activity& activity : catalog.activities) {
        referenceCount += activity.graphHashes.size();
        if (referenceCount > kMaximumReferences) {
            return fail(error, "activity graph references exceed the supported maximum");
        }
        for (const std::uint32_t graphHash : activity.graphHashes) {
            if (!graph_exists(graphHashes, graphHash)) {
                return fail(error, "activity references an unknown graph");
            }
        }
    }

    std::size_t nodeCount = 0;
    for (const Graph& graph : catalog.graphs) {
        nodeCount += graph.nodes.size();
        referenceCount += graph.linkedGraphHashes.size();
        if (nodeCount > kMaximumNodes || referenceCount > kMaximumReferences) {
            return fail(error, "graph node or link count exceeds the supported maximum");
        }
        for (const std::uint32_t linkedGraph : graph.linkedGraphHashes) {
            if (!graph_exists(graphHashes, linkedGraph)) {
                return fail(error, "graph links to an unknown graph");
            }
        }
        for (const GraphNode& node : graph.nodes) {
            if (node.graphHash != graph.hash || node.nodeHash == 0 || !std::isfinite(node.authoredX)
                || !std::isfinite(node.authoredY)) {
                return fail(error, "graph node identity or authored position is invalid");
            }
            const std::uint64_t key =
                (static_cast<std::uint64_t>(node.graphHash) << 32U) | node.nodeHash;
            if (!nodeKeys.insert(key).second) {
                return fail(error, "duplicate graph node key");
            }
            referenceCount += node.activityHashes.size() + node.linkedGraphHashes.size();
            if (referenceCount > kMaximumReferences) {
                return fail(error, "node reference count exceeds the supported maximum");
            }
            for (const std::uint32_t activityHash : node.activityHashes) {
                if (!graph_exists(activityHashes, activityHash)) {
                    return fail(error, "graph node references an unknown activity");
                }
            }
            for (const std::uint32_t linkedGraph : node.linkedGraphHashes) {
                if (!graph_exists(graphHashes, linkedGraph)) {
                    return fail(error, "graph node links to an unknown graph");
                }
            }
        }
    }

    for (const LocationRelease& release : catalog.locationReleases) {
        for (const float lane : release.spawnPoint) {
            if (!std::isfinite(lane)) {
                return fail(error, "location spawn point is not finite");
            }
        }
        for (const float lane : release.publicPosition) {
            if (!std::isfinite(lane)) {
                return fail(error, "location public position is not finite");
            }
        }
        const Graph* graph = find_graph(catalog, release.graphHash);
        if (graph == nullptr || find_node(*graph, release.nodeHash) == nullptr) {
            return fail(error, "location release references an unknown graph node");
        }
    }
    return true;
}

bool load(std::span<const std::byte> bytes, Catalog& catalog, std::string& error) {
    try {
        catalog = {};
        error.clear();
        if (bytes.size() < kHeaderSize || bytes.size() > kMaximumFileSize) {
            return fail(error, "catalog file size is invalid");
        }
        const Reader reader{bytes};
        for (std::size_t index = 0; index < kMagic.size(); ++index) {
            if (std::to_integer<std::uint8_t>(bytes[index]) != kMagic[index]) {
                return fail(error, "catalog magic is invalid");
            }
        }
        std::uint32_t schema{};
        std::uint32_t contentBuild{};
        std::uint32_t headerSize{};
        std::uint32_t totalSize{};
        std::uint32_t versionOffset{};
        std::uint32_t versionSize{};
        std::uint32_t stringOffset{};
        std::uint32_t stringSize{};
        if (!reader.read_u32(8, schema) || !reader.read_u32(12, contentBuild)
            || !reader.read_u32(16, headerSize) || !reader.read_u32(20, totalSize)
            || !reader.read_u32(24, versionOffset) || !reader.read_u32(28, versionSize)
            || !reader.read_u32(32, stringOffset) || !reader.read_u32(36, stringSize)) {
            return fail(error, "catalog header is truncated");
        }
        if (schema != kSchemaVersion || headerSize != kHeaderSize || totalSize != bytes.size()
            || headerSize > totalSize) {
            return fail(error, "catalog header version or size is invalid");
        }
        std::array<Section, kSectionCount> sections{};
        std::size_t sectionOffset = 40;
        for (std::size_t index = 0; index < sections.size(); ++index) {
            if (!reader.read_u32(sectionOffset, sections[index].offset)
                || !reader.read_u32(sectionOffset + 4U, sections[index].count)
                || !reader.read_u32(sectionOffset + 8U, sections[index].stride)
                || sections[index].stride != kExpectedStrides[index]) {
                return fail(error, "catalog section descriptor is invalid");
            }
            sectionOffset += 12U;
        }
        if (!safe_range(bytes.size(), stringOffset, stringSize, 1, kMaximumFileSize)
            || !safe_range(bytes.size(),
                           sections[kActivitySection].offset,
                           sections[kActivitySection].count,
                           sections[kActivitySection].stride,
                           kMaximumActivities)
            || !safe_range(bytes.size(),
                           sections[kGraphSection].offset,
                           sections[kGraphSection].count,
                           sections[kGraphSection].stride,
                           kMaximumGraphs)
            || !safe_range(bytes.size(),
                           sections[kNodeSection].offset,
                           sections[kNodeSection].count,
                           sections[kNodeSection].stride,
                           kMaximumNodes)
            || !safe_range(bytes.size(),
                           sections[kActivityRefSection].offset,
                           sections[kActivityRefSection].count,
                           sections[kActivityRefSection].stride,
                           kMaximumReferences)
            || !safe_range(bytes.size(),
                           sections[kLinkedRefSection].offset,
                           sections[kLinkedRefSection].count,
                           sections[kLinkedRefSection].stride,
                           kMaximumReferences)
            || !safe_range(bytes.size(),
                           sections[kLocationSection].offset,
                           sections[kLocationSection].count,
                           sections[kLocationSection].stride,
                           kMaximumLocations)) {
            return fail(error, "catalog section range is invalid");
        }
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        ranges.reserve(kSectionCount + 1U);
        const auto add_range =
            [&](std::uint32_t offset, std::uint32_t count, std::uint32_t stride) {
                if (count == 0) {
                    return true;
                }
                if (offset < headerSize) {
                    return false;
                }
                const std::size_t begin = offset;
                const std::size_t end = begin + static_cast<std::size_t>(count) * stride;
                ranges.emplace_back(begin, end);
                return true;
            };
        for (const Section& section : sections) {
            if (!add_range(section.offset, section.count, section.stride)) {
                return fail(error, "catalog section overlaps its header");
            }
        }
        if (stringSize == 0 || stringOffset < headerSize) {
            return fail(error, "catalog string table range is invalid");
        }
        ranges.emplace_back(stringOffset, static_cast<std::size_t>(stringOffset) + stringSize);
        std::ranges::sort(ranges);
        for (std::size_t index = 1; index < ranges.size(); ++index) {
            if (ranges[index - 1U].second > ranges[index].first) {
                return fail(error, "catalog sections overlap");
            }
        }
        const Section strings{stringOffset, stringSize, 1};
        if (versionSize == 0
            || !string_at(reader, strings, versionOffset, versionSize, catalog.manifestVersion)) {
            return fail(error, "catalog version string is invalid");
        }
        if (!reader.read_bytes(124, catalog.activityDigest)
            || !reader.read_bytes(156, catalog.graphDigest)
            || !reader.read_bytes(188, catalog.locationDigest)) {
            return fail(error, "catalog source digest block is truncated");
        }
        catalog.contentBuild = contentBuild;

        const auto record_offset = [](const Section& section, std::size_t index) noexcept {
            return static_cast<std::size_t>(section.offset)
                   + index * static_cast<std::size_t>(section.stride);
        };
        std::vector<std::uint32_t> activityRefs(sections[kActivityRefSection].count);
        for (std::size_t index = 0; index < activityRefs.size(); ++index) {
            if (!reader.read_u32(record_offset(sections[kActivityRefSection], index),
                                 activityRefs[index])) {
                return fail(error, "activity reference section is truncated");
            }
        }
        std::vector<std::uint32_t> linkedRefs(sections[kLinkedRefSection].count);
        for (std::size_t index = 0; index < linkedRefs.size(); ++index) {
            if (!reader.read_u32(record_offset(sections[kLinkedRefSection], index),
                                 linkedRefs[index])) {
                return fail(error, "linked graph section is truncated");
            }
        }

        catalog.activities.reserve(sections[kActivitySection].count);
        for (std::size_t index = 0; index < sections[kActivitySection].count; ++index) {
            const std::size_t offset = record_offset(sections[kActivitySection], index);
            Activity activity{};
            std::uint32_t nameOffset{};
            std::uint32_t nameLength{};
            std::uint32_t graphStart{};
            std::uint32_t graphCount{};
            if (!reader.read_u32(offset, activity.hash) || !reader.read_u32(offset + 4U, nameOffset)
                || !reader.read_u32(offset + 8U, nameLength)
                || !reader.read_u32(offset + 12U, graphStart)
                || !reader.read_u32(offset + 16U, graphCount) || graphStart > activityRefs.size()
                || graphCount > activityRefs.size() - graphStart
                || !string_at(reader, strings, nameOffset, nameLength, activity.name)) {
                return fail(error, "activity record is invalid");
            }
            activity.graphHashes.assign(activityRefs.begin() + graphStart,
                                        activityRefs.begin() + graphStart + graphCount);
            catalog.activities.push_back(std::move(activity));
        }

        catalog.graphs.reserve(sections[kGraphSection].count);
        std::vector<std::uint32_t> graphNodeStarts;
        std::vector<std::uint32_t> graphNodeCounts;
        graphNodeStarts.reserve(sections[kGraphSection].count);
        graphNodeCounts.reserve(sections[kGraphSection].count);
        for (std::size_t index = 0; index < sections[kGraphSection].count; ++index) {
            const std::size_t offset = record_offset(sections[kGraphSection], index);
            Graph graph{};
            std::uint32_t nodeStart{};
            std::uint32_t nodeCount{};
            std::uint32_t linkedStart{};
            std::uint32_t linkedCount{};
            if (!reader.read_u32(offset, graph.hash) || !reader.read_u32(offset + 4U, nodeStart)
                || !reader.read_u32(offset + 8U, nodeCount)
                || !reader.read_u32(offset + 12U, linkedStart)
                || !reader.read_u32(offset + 16U, linkedCount)
                || nodeStart > sections[kNodeSection].count
                || nodeCount > sections[kNodeSection].count - nodeStart
                || linkedStart > linkedRefs.size()
                || linkedCount > linkedRefs.size() - linkedStart) {
                return fail(error, "graph record is invalid");
            }
            graph.linkedGraphHashes.assign(linkedRefs.begin() + linkedStart,
                                           linkedRefs.begin() + linkedStart + linkedCount);
            catalog.graphs.push_back(std::move(graph));
            graphNodeStarts.push_back(nodeStart);
            graphNodeCounts.push_back(nodeCount);
        }
        std::vector<std::uint32_t> expectedNodeGraphs(sections[kNodeSection].count);
        for (std::size_t graphIndex = 0; graphIndex < catalog.graphs.size(); ++graphIndex) {
            const std::size_t start = graphNodeStarts[graphIndex];
            const std::size_t count = graphNodeCounts[graphIndex];
            for (std::size_t nodeIndex = start; nodeIndex < start + count; ++nodeIndex) {
                if (expectedNodeGraphs[nodeIndex] != 0) {
                    return fail(error, "graph node ranges overlap");
                }
                expectedNodeGraphs[nodeIndex] = catalog.graphs[graphIndex].hash;
            }
        }

        for (std::size_t index = 0; index < sections[kNodeSection].count; ++index) {
            const std::size_t offset = record_offset(sections[kNodeSection], index);
            GraphNode node{};
            std::uint32_t activityStart{};
            std::uint32_t activityCount{};
            std::uint32_t linkedStart{};
            std::uint32_t linkedCount{};
            if (!reader.read_u32(offset, node.graphHash)
                || !reader.read_u32(offset + 4U, node.nodeHash)
                || !reader.read_f32(offset + 8U, node.authoredX)
                || !reader.read_f32(offset + 12U, node.authoredY)
                || !reader.read_u32(offset + 16U, node.stateHash)
                || !reader.read_u32(offset + 20U, node.styleHash)
                || !reader.read_u32(offset + 24U, activityStart)
                || !reader.read_u32(offset + 28U, activityCount)
                || !reader.read_u32(offset + 32U, linkedStart)
                || !reader.read_u32(offset + 36U, linkedCount)
                || activityStart > activityRefs.size()
                || activityCount > activityRefs.size() - activityStart
                || linkedStart > linkedRefs.size()
                || linkedCount > linkedRefs.size() - linkedStart) {
                return fail(error, "graph node record is invalid");
            }
            if (expectedNodeGraphs[index] != node.graphHash) {
                return fail(error, "graph node is outside its owner range");
            }
            node.activityHashes.assign(activityRefs.begin() + activityStart,
                                       activityRefs.begin() + activityStart + activityCount);
            node.linkedGraphHashes.assign(linkedRefs.begin() + linkedStart,
                                          linkedRefs.begin() + linkedStart + linkedCount);
            Graph* graph = nullptr;
            for (Graph& candidate : catalog.graphs) {
                if (candidate.hash == node.graphHash) {
                    graph = &candidate;
                    break;
                }
            }
            if (graph == nullptr) {
                return fail(error, "graph node references an unknown owner graph");
            }
            graph->nodes.push_back(std::move(node));
        }

        catalog.locationReleases.reserve(sections[kLocationSection].count);
        for (std::size_t index = 0; index < sections[kLocationSection].count; ++index) {
            const std::size_t offset = record_offset(sections[kLocationSection], index);
            LocationRelease release{};
            if (!reader.read_u32(offset, release.locationHash)
                || !reader.read_u32(offset + 4U, release.graphHash)
                || !reader.read_u32(offset + 8U, release.nodeHash)
                || !reader.read_f32(offset + 12U, release.spawnPoint[0])
                || !reader.read_f32(offset + 16U, release.spawnPoint[1])
                || !reader.read_f32(offset + 20U, release.spawnPoint[2])
                || !reader.read_f32(offset + 24U, release.publicPosition[0])
                || !reader.read_f32(offset + 28U, release.publicPosition[1])
                || !reader.read_f32(offset + 32U, release.publicPosition[2])
                || !reader.read_f32(offset + 36U, release.publicPosition[3])) {
                return fail(error, "location release record is invalid");
            }
            catalog.locationReleases.push_back(release);
        }

        if (!validate(catalog, error)) {
            catalog = {};
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        catalog = {};
        error = exception.what();
        return false;
    } catch (...) {
        catalog = {};
        error = "catalog parser failed";
        return false;
    }
}

LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept {
    LoadResult result{};
    catalog = {};
    try {
        const std::filesystem::path file(path);
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input) {
            result.compatibility = Compatibility::missing;
            result.diagnostic = "optional activity catalog is absent";
            return result;
        }
        const std::streampos end = input.tellg();
        if (end <= 0 || static_cast<std::uint64_t>(end) > kMaximumFileSize) {
            result.compatibility = Compatibility::malformed;
            result.diagnostic = "activity catalog file size is invalid";
            return result;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            result.compatibility = Compatibility::malformed;
            result.diagnostic = "activity catalog read failed";
            return result;
        }
        std::string error;
        if (!load(bytes, catalog, error)) {
            result.compatibility = Compatibility::malformed;
            result.diagnostic = std::move(error);
            return result;
        }
        result.compatibility = compatibility(catalog);
        result.diagnostic = result.compatibility == Compatibility::compatible
                                ? "activity catalog loaded"
                                : "activity catalog is browse-only for another content build";
        return result;
    } catch (...) {
        catalog = {};
        result.compatibility = Compatibility::malformed;
        result.diagnostic = "activity catalog load failed";
        return result;
    }
}

Compatibility compatibility(const Catalog& catalog) noexcept {
    return catalog.contentBuild == 0                     ? Compatibility::malformed
           : catalog.contentBuild == kTargetContentBuild ? Compatibility::compatible
                                                         : Compatibility::buildMismatch;
}

const Activity* find_activity(const Catalog& catalog, std::uint32_t hash) noexcept {
    const auto iterator = std::ranges::find(catalog.activities, hash, &Activity::hash);
    return iterator == catalog.activities.end() ? nullptr : &*iterator;
}

const Graph* find_graph(const Catalog& catalog, std::uint32_t hash) noexcept {
    const auto iterator = std::ranges::find(catalog.graphs, hash, &Graph::hash);
    return iterator == catalog.graphs.end() ? nullptr : &*iterator;
}

const GraphNode* find_node(const Graph& graph, std::uint32_t nodeHash) noexcept {
    const auto iterator = std::ranges::find(graph.nodes, nodeHash, &GraphNode::nodeHash);
    return iterator == graph.nodes.end() ? nullptr : &*iterator;
}

} // namespace sunrise::client::inspection::activity_catalog
