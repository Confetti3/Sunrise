#include "activity_logic_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace sunrise::client::inspection::activity_logic_catalog {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'S', 'L', 'O', 'G', 'I', 'C', '0', '1'};
constexpr std::size_t kSectionCount = 5;
constexpr std::array<std::uint32_t, kSectionCount> kExpectedStrides{28, 48, 4, 44, 16};
constexpr std::size_t kActivitySection = 0;
constexpr std::size_t kEntitySection = 1;
constexpr std::size_t kActivityRefSection = 2;
constexpr std::size_t kPlacementSection = 3;
constexpr std::size_t kEdgeSection = 4;
constexpr std::size_t kMaximumFileSize = 128U * 1024U * 1024U;
constexpr std::uint32_t kMaximumActivities = 4096;
constexpr std::uint32_t kMaximumEntities = 250000;
constexpr std::uint32_t kMaximumActivityRefs = 2000000;
constexpr std::uint32_t kMaximumPlacements = 500000;
constexpr std::uint32_t kMaximumEdges = 250000;

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

    [[nodiscard]] bool read_u16(std::size_t offset, std::uint16_t& value) const noexcept {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint16_t)) {
            return false;
        }
        value = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset]))
                | static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
                    << 8U);
        return true;
    }

    [[nodiscard]] bool read_u32(std::size_t offset, std::uint32_t& value) const noexcept {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
            return false;
        }
        value =
            static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset]))
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U)
            | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]))
               << 24U);
        return true;
    }

    [[nodiscard]] bool read_u64(std::size_t offset, std::uint64_t& value) const noexcept {
        std::uint32_t low{};
        std::uint32_t high{};
        if (!read_u32(offset, low) || !read_u32(offset + 4U, high)) {
            return false;
        }
        value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32U);
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

[[nodiscard]] bool string_at(const Reader& reader,
                             std::uint32_t stringOffset,
                             std::uint32_t stringSize,
                             std::uint32_t relativeOffset,
                             std::uint32_t length,
                             std::string& output) {
    if (relativeOffset > stringSize || length > stringSize - relativeOffset) {
        return false;
    }
    const std::size_t absolute = static_cast<std::size_t>(stringOffset) + relativeOffset;
    if (absolute > reader.bytes.size() || length > reader.bytes.size() - absolute) {
        return false;
    }
    const std::span<const std::byte> bytes = reader.bytes.subspan(absolute, length);
    for (const std::byte value : bytes) {
        if (std::to_integer<std::uint8_t>(value) == 0U) {
            return false;
        }
    }
    output.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

[[nodiscard]] bool fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

[[nodiscard]] bool role_valid(Role role) noexcept {
    const auto value = static_cast<std::uint8_t>(role);
    return value <= static_cast<std::uint8_t>(Role::triggerSource) || role == Role::unknown;
}

[[nodiscard]] bool confidence_valid(Confidence confidence) noexcept {
    return static_cast<std::uint8_t>(confidence) <= static_cast<std::uint8_t>(Confidence::strong);
}

/** Builds outgoing/incoming edge indexes once after parsing. */
void build_adjacency(Catalog& catalog) {
    const std::size_t entityCount = catalog.entities.size();
    catalog.edgeBySourceOffsets.assign(entityCount + 1U, 0U);
    catalog.edgeByTargetOffsets.assign(entityCount + 1U, 0U);

    // Count degree per endpoint.
    for (std::size_t edge = 0; edge < catalog.edges.size(); ++edge) {
        const std::uint32_t source = catalog.edges[edge].sourceEntityIndex;
        const std::uint32_t target = catalog.edges[edge].targetEntityIndex;
        if (source < entityCount) {
            ++catalog.edgeBySourceOffsets[source + 1U];
        }
        if (target < entityCount) {
            ++catalog.edgeByTargetOffsets[target + 1U];
        }
    }

    // Prefix sums -> per-entity start offsets.
    for (std::size_t entity = 0; entity < entityCount; ++entity) {
        catalog.edgeBySourceOffsets[entity + 1U] += catalog.edgeBySourceOffsets[entity];
        catalog.edgeByTargetOffsets[entity + 1U] += catalog.edgeByTargetOffsets[entity];
    }

    catalog.edgeBySource.assign(catalog.edges.size(), 0U);
    catalog.edgeByTarget.assign(catalog.edges.size(), 0U);
    std::vector<std::uint32_t> sourceCursor(catalog.edgeBySourceOffsets);
    std::vector<std::uint32_t> targetCursor(catalog.edgeByTargetOffsets);
    for (std::uint32_t edge = 0; edge < catalog.edges.size(); ++edge) {
        const std::uint32_t source = catalog.edges[edge].sourceEntityIndex;
        const std::uint32_t target = catalog.edges[edge].targetEntityIndex;
        if (source < entityCount) {
            catalog.edgeBySource[sourceCursor[source]++] = edge;
        }
        if (target < entityCount) {
            catalog.edgeByTarget[targetCursor[target]++] = edge;
        }
    }
}

} // namespace

bool validate(const Catalog& catalog, std::string& error) {
    error.clear();
    if (catalog.activities.empty() || catalog.entities.empty()) {
        return fail(error, "activity logic catalog is empty");
    }
    if (catalog.activities.size() > kMaximumActivities || catalog.entities.size() > kMaximumEntities
        || catalog.edges.size() > kMaximumEdges) {
        return fail(error, "activity logic catalog exceeds supported count limits");
    }

    std::unordered_set<std::uint32_t> scenarios;
    std::unordered_set<std::uint32_t> definitions;
    scenarios.reserve(catalog.activities.size());
    definitions.reserve(catalog.entities.size());

    std::size_t referenceCount = 0;
    std::size_t placementCount = 0;
    for (const Entity& entity : catalog.entities) {
        if (entity.definitionTag == 0 || entity.classPrimary == 0 || entity.classSecondary == 0
            || !definitions.insert(entity.definitionTag).second || !role_valid(entity.role)
            || !confidence_valid(entity.confidence) || entity.name.empty()
            || entity.label.empty()) {
            return fail(error, "activity logic entity identity is invalid");
        }
        placementCount += entity.placements.size();
        if (placementCount > kMaximumPlacements) {
            return fail(error, "activity logic placement count exceeds supported maximum");
        }
        for (const Placement& placement : entity.placements) {
            if (placement.worldId == 0) {
                return fail(error, "activity logic placement WorldID is zero");
            }
            for (const float lane : placement.position) {
                if (!std::isfinite(lane)) {
                    return fail(error, "activity logic placement position is not finite");
                }
            }
            for (const float lane : placement.rotation) {
                if (!std::isfinite(lane)) {
                    return fail(error, "activity logic placement rotation is not finite");
                }
            }
        }
    }

    for (const Activity& activity : catalog.activities) {
        if (activity.scenarioTag == 0 || activity.name.empty()
            || !scenarios.insert(activity.scenarioTag).second) {
            return fail(error, "activity logic activity identity is invalid");
        }
        referenceCount += activity.entityIndices.size();
        if (referenceCount > kMaximumActivityRefs) {
            return fail(error, "activity logic activity-reference count exceeds supported maximum");
        }
        for (const std::uint32_t index : activity.entityIndices) {
            if (index >= catalog.entities.size()) {
                return fail(error, "activity logic activity references an unknown entity");
            }
        }
    }

    for (const Edge& edge : catalog.edges) {
        if (edge.sourceEntityIndex >= catalog.entities.size()
            || edge.targetEntityIndex >= catalog.entities.size()) {
            return fail(error, "activity logic edge references an unknown entity");
        }
    }
    return true;
}

bool load(std::span<const std::byte> bytes, Catalog& catalog, std::string& error) {
    try {
        catalog = {};
        error.clear();
        if (bytes.size() < kHeaderSize || bytes.size() > kMaximumFileSize) {
            return fail(error, "activity logic catalog file size is invalid");
        }
        for (std::size_t index = 0; index < kMagic.size(); ++index) {
            if (std::to_integer<std::uint8_t>(bytes[index]) != kMagic[index]) {
                return fail(error, "activity logic catalog magic is invalid");
            }
        }

        const Reader reader{bytes};
        std::uint32_t schema{};
        std::uint32_t headerSize{};
        std::uint32_t totalSize{};
        std::uint32_t stringOffset{};
        std::uint32_t stringSize{};
        if (!reader.read_u32(8, schema) || !reader.read_u32(12, headerSize)
            || !reader.read_u32(16, totalSize) || !reader.read_u32(20, stringOffset)
            || !reader.read_u32(24, stringSize)) {
            return fail(error, "activity logic catalog header is truncated");
        }
        if (schema != kSchemaVersion || headerSize != kHeaderSize || totalSize != bytes.size()
            || headerSize > totalSize) {
            return fail(error, "activity logic catalog schema or size is invalid");
        }
        if (!safe_range(bytes.size(), stringOffset, stringSize, 1, kMaximumFileSize)
            || stringOffset < headerSize) {
            return fail(error, "activity logic string table is invalid");
        }
        for (std::size_t index = 0; index < catalog.provenance.sourceDigest.size(); ++index) {
            std::uint8_t value{};
            if (!reader.read_u8(28U + index, value)) {
                return fail(error, "activity logic source digest is truncated");
            }
            catalog.provenance.sourceDigest[index] = value;
        }
        std::uint32_t converterVersion{};
        std::uint32_t contentBuild{};
        std::uint64_t generationTimestamp{};
        std::uint32_t sourceFormatOffset{};
        std::uint32_t sourceFormatLength{};
        if (!reader.read_u32(60, converterVersion) || !reader.read_u32(124, contentBuild)
            || !reader.read_u64(128, generationTimestamp)
            || !reader.read_u32(136, sourceFormatOffset)
            || !reader.read_u32(140, sourceFormatLength)) {
            return fail(error, "activity logic provenance header is truncated");
        }
        if (converterVersion != kConverterVersion) {
            return fail(error, "activity logic converter version is unsupported");
        }
        catalog.provenance.converterVersion = converterVersion;
        catalog.provenance.contentBuild = contentBuild;
        catalog.provenance.generationTimestamp = generationTimestamp;
        if (!string_at(reader,
                       stringOffset,
                       stringSize,
                       sourceFormatOffset,
                       sourceFormatLength,
                       catalog.provenance.sourceFormat)) {
            return fail(error, "activity logic provenance source format is invalid");
        }

        std::array<Section, kSectionCount> sections{};
        std::size_t descriptor = 64;
        for (std::size_t index = 0; index < sections.size(); ++index) {
            if (!reader.read_u32(descriptor, sections[index].offset)
                || !reader.read_u32(descriptor + 4U, sections[index].count)
                || !reader.read_u32(descriptor + 8U, sections[index].stride)
                || sections[index].stride != kExpectedStrides[index]) {
                return fail(error, "activity logic section descriptor is invalid");
            }
            descriptor += 12U;
        }

        if (!safe_range(bytes.size(),
                        sections[kActivitySection].offset,
                        sections[kActivitySection].count,
                        sections[kActivitySection].stride,
                        kMaximumActivities)
            || !safe_range(bytes.size(),
                           sections[kEntitySection].offset,
                           sections[kEntitySection].count,
                           sections[kEntitySection].stride,
                           kMaximumEntities)
            || !safe_range(bytes.size(),
                           sections[kActivityRefSection].offset,
                           sections[kActivityRefSection].count,
                           sections[kActivityRefSection].stride,
                           kMaximumActivityRefs)
            || !safe_range(bytes.size(),
                           sections[kPlacementSection].offset,
                           sections[kPlacementSection].count,
                           sections[kPlacementSection].stride,
                           kMaximumPlacements)
            || !safe_range(bytes.size(),
                           sections[kEdgeSection].offset,
                           sections[kEdgeSection].count,
                           sections[kEdgeSection].stride,
                           kMaximumEdges)) {
            return fail(error, "activity logic section range is invalid");
        }

        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        ranges.reserve(kSectionCount + 1U);
        for (const Section& section : sections) {
            if (section.count == 0) {
                continue;
            }
            if (section.offset < headerSize) {
                return fail(error, "activity logic section overlaps its header");
            }
            ranges.emplace_back(section.offset,
                                static_cast<std::size_t>(section.offset)
                                    + static_cast<std::size_t>(section.count) * section.stride);
        }
        ranges.emplace_back(stringOffset, static_cast<std::size_t>(stringOffset) + stringSize);
        std::ranges::sort(ranges);
        for (std::size_t index = 1; index < ranges.size(); ++index) {
            if (ranges[index - 1U].second > ranges[index].first) {
                return fail(error, "activity logic catalog sections overlap");
            }
        }

        const Section& entitySection = sections[kEntitySection];
        const Section& placementSection = sections[kPlacementSection];
        catalog.entities.reserve(entitySection.count);
        for (std::uint32_t index = 0; index < entitySection.count; ++index) {
            const std::size_t offset =
                entitySection.offset + static_cast<std::size_t>(index) * entitySection.stride;
            Entity entity{};
            std::uint8_t role{};
            std::uint8_t confidence{};
            std::uint16_t reserved{};
            std::uint32_t nameOffset{};
            std::uint32_t nameLength{};
            std::uint32_t labelOffset{};
            std::uint32_t labelLength{};
            std::uint32_t localizedOffset{};
            std::uint32_t localizedLength{};
            std::uint32_t firstPlacement{};
            std::uint32_t placementCount{};
            if (!reader.read_u32(offset, entity.definitionTag)
                || !reader.read_u32(offset + 4U, entity.classPrimary)
                || !reader.read_u32(offset + 8U, entity.classSecondary)
                || !reader.read_u8(offset + 12U, role) || !reader.read_u8(offset + 13U, confidence)
                || !reader.read_u16(offset + 14U, reserved)
                || !reader.read_u32(offset + 16U, nameOffset)
                || !reader.read_u32(offset + 20U, nameLength)
                || !reader.read_u32(offset + 24U, labelOffset)
                || !reader.read_u32(offset + 28U, labelLength)
                || !reader.read_u32(offset + 32U, localizedOffset)
                || !reader.read_u32(offset + 36U, localizedLength)
                || !reader.read_u32(offset + 40U, firstPlacement)
                || !reader.read_u32(offset + 44U, placementCount)) {
                return fail(error, "activity logic entity record is truncated");
            }
            entity.role = static_cast<Role>(role);
            entity.confidence = static_cast<Confidence>(confidence);
            if (!role_valid(entity.role) || !confidence_valid(entity.confidence)
                || !string_at(reader, stringOffset, stringSize, nameOffset, nameLength, entity.name)
                || !string_at(
                    reader, stringOffset, stringSize, labelOffset, labelLength, entity.label)
                || !string_at(reader,
                              stringOffset,
                              stringSize,
                              localizedOffset,
                              localizedLength,
                              entity.localizedText)
                || firstPlacement > placementSection.count
                || placementCount > placementSection.count - firstPlacement) {
                return fail(error, "activity logic entity record is invalid");
            }
            entity.placements.reserve(placementCount);
            for (std::uint32_t placementIndex = 0; placementIndex < placementCount;
                 ++placementIndex) {
                const std::size_t placementOffset =
                    placementSection.offset
                    + static_cast<std::size_t>(firstPlacement + placementIndex)
                          * placementSection.stride;
                Placement placement{};
                if (!reader.read_u64(placementOffset, placement.worldId)
                    || !reader.read_u32(placementOffset + 8U, placement.mapTableTag)
                    || !reader.read_u32(placementOffset + 12U, placement.placedEntityTag)) {
                    return fail(error, "activity logic placement record is truncated");
                }
                for (std::size_t lane = 0; lane < placement.position.size(); ++lane) {
                    if (!reader.read_f32(placementOffset + 16U + lane * 4U,
                                         placement.position[lane])) {
                        return fail(error, "activity logic placement position is truncated");
                    }
                }
                for (std::size_t lane = 0; lane < placement.rotation.size(); ++lane) {
                    if (!reader.read_f32(placementOffset + 28U + lane * 4U,
                                         placement.rotation[lane])) {
                        return fail(error, "activity logic placement rotation is truncated");
                    }
                }
                entity.placements.push_back(placement);
            }
            catalog.entities.push_back(std::move(entity));
        }

        const Section& refSection = sections[kActivityRefSection];
        const Section& activitySection = sections[kActivitySection];
        catalog.activities.reserve(activitySection.count);
        for (std::uint32_t index = 0; index < activitySection.count; ++index) {
            const std::size_t offset =
                activitySection.offset + static_cast<std::size_t>(index) * activitySection.stride;
            Activity activity{};
            std::uint32_t nameOffset{};
            std::uint32_t nameLength{};
            std::uint32_t destinationOffset{};
            std::uint32_t destinationLength{};
            std::uint32_t firstReference{};
            std::uint32_t referenceCount{};
            if (!reader.read_u32(offset, activity.scenarioTag)
                || !reader.read_u32(offset + 4U, nameOffset)
                || !reader.read_u32(offset + 8U, nameLength)
                || !reader.read_u32(offset + 12U, destinationOffset)
                || !reader.read_u32(offset + 16U, destinationLength)
                || !reader.read_u32(offset + 20U, firstReference)
                || !reader.read_u32(offset + 24U, referenceCount)
                || !string_at(
                    reader, stringOffset, stringSize, nameOffset, nameLength, activity.name)
                || !string_at(reader,
                              stringOffset,
                              stringSize,
                              destinationOffset,
                              destinationLength,
                              activity.destination)
                || firstReference > refSection.count
                || referenceCount > refSection.count - firstReference) {
                return fail(error, "activity logic activity record is invalid");
            }
            activity.entityIndices.reserve(referenceCount);
            for (std::uint32_t reference = 0; reference < referenceCount; ++reference) {
                std::uint32_t entityIndex{};
                const std::size_t refOffset =
                    refSection.offset
                    + static_cast<std::size_t>(firstReference + reference) * refSection.stride;
                if (!reader.read_u32(refOffset, entityIndex)
                    || entityIndex >= catalog.entities.size()) {
                    return fail(error, "activity logic activity reference is invalid");
                }
                activity.entityIndices.push_back(entityIndex);
            }
            catalog.activities.push_back(std::move(activity));
        }

        const Section& edgeSection = sections[kEdgeSection];
        catalog.edges.reserve(edgeSection.count);
        for (std::uint32_t index = 0; index < edgeSection.count; ++index) {
            const std::size_t offset =
                edgeSection.offset + static_cast<std::size_t>(index) * edgeSection.stride;
            Edge edge{};
            if (!reader.read_u32(offset, edge.sourceEntityIndex)
                || !reader.read_u32(offset + 4U, edge.targetEntityIndex)
                || !reader.read_u32(offset + 8U, edge.nameHash)
                || !reader.read_u32(offset + 12U, edge.occurrenceCount)) {
                return fail(error, "activity logic edge record is truncated");
            }
            catalog.edges.push_back(edge);
        }

        if (!validate(catalog, error)) {
            return false;
        }
        build_adjacency(catalog);
        return true;
    } catch (...) {
        catalog = {};
        return fail(error, "activity logic catalog allocation or parse failed");
    }
}

LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept {
    LoadResult result{};
    try {
        const std::filesystem::path filePath(path);
        if (!std::filesystem::exists(filePath)) {
            catalog = {};
            result.state = LoadState::missing;
            result.diagnostic = "No optional activity logic catalog is installed.";
            return result;
        }
        const std::uintmax_t size = std::filesystem::file_size(filePath);
        if (size < kHeaderSize || size > kMaximumFileSize) {
            catalog = {};
            result.state = LoadState::malformed;
            result.diagnostic = "Activity logic catalog file size is invalid.";
            return result;
        }
        std::ifstream stream(filePath, std::ios::binary);
        if (!stream) {
            catalog = {};
            result.state = LoadState::malformed;
            result.diagnostic = "Activity logic catalog could not be opened.";
            return result;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!stream || static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
            catalog = {};
            result.state = LoadState::malformed;
            result.diagnostic = "Activity logic catalog could not be read completely.";
            return result;
        }
        std::string error;
        if (!load(bytes, catalog, error)) {
            result.state = LoadState::malformed;
            result.diagnostic = "Activity logic catalog rejected: " + error;
            return result;
        }
        result.state = LoadState::ready;
        result.diagnostic = "Activity logic catalog loaded.";
        return result;
    } catch (...) {
        catalog = {};
        result.state = LoadState::malformed;
        result.diagnostic = "Activity logic catalog load failed unexpectedly.";
        return result;
    }
}

const Activity* find_activity(const Catalog& catalog, std::uint32_t scenarioTag) noexcept {
    const auto iterator = std::lower_bound(
        catalog.activities.begin(),
        catalog.activities.end(),
        scenarioTag,
        [](const Activity& activity, std::uint32_t value) { return activity.scenarioTag < value; });
    return iterator != catalog.activities.end() && iterator->scenarioTag == scenarioTag ? &*iterator
                                                                                        : nullptr;
}

const Entity* find_entity(const Catalog& catalog, std::uint32_t definitionTag) noexcept {
    const auto iterator = std::lower_bound(
        catalog.entities.begin(),
        catalog.entities.end(),
        definitionTag,
        [](const Entity& entity, std::uint32_t value) { return entity.definitionTag < value; });
    return iterator != catalog.entities.end() && iterator->definitionTag == definitionTag
               ? &*iterator
               : nullptr;
}

std::span<const std::uint32_t> outgoing_edges(const Catalog& catalog,
                                              std::uint32_t entityIndex) noexcept {
    if (entityIndex >= catalog.entities.size()
        || catalog.edgeBySourceOffsets.size() != catalog.entities.size() + 1U) {
        return {};
    }
    const std::uint32_t begin = catalog.edgeBySourceOffsets[entityIndex];
    const std::uint32_t end = catalog.edgeBySourceOffsets[entityIndex + 1U];
    if (begin >= end || end > catalog.edgeBySource.size()) {
        return {};
    }
    return std::span<const std::uint32_t>(catalog.edgeBySource.data() + begin, end - begin);
}

std::span<const std::uint32_t> incoming_edges(const Catalog& catalog,
                                              std::uint32_t entityIndex) noexcept {
    if (entityIndex >= catalog.entities.size()
        || catalog.edgeByTargetOffsets.size() != catalog.entities.size() + 1U) {
        return {};
    }
    const std::uint32_t begin = catalog.edgeByTargetOffsets[entityIndex];
    const std::uint32_t end = catalog.edgeByTargetOffsets[entityIndex + 1U];
    if (begin >= end || end > catalog.edgeByTarget.size()) {
        return {};
    }
    return std::span<const std::uint32_t>(catalog.edgeByTarget.data() + begin, end - begin);
}

const char* role_name(Role role) noexcept {
    switch (role) {
    case Role::actionSequence:
        return "Action sequence";
    case Role::actionTarget:
        return "Action target";
    case Role::competitiveRule:
        return "Competitive rule";
    case Role::conditionMonitor:
        return "Condition monitor";
    case Role::device:
        return "Device";
    case Role::object:
        return "Object / interactable";
    case Role::objective:
        return "Objective";
    case Role::spatialRule:
        return "Spatial rule / area";
    case Role::spawnDefinition:
        return "Squad spawn rule";
    case Role::squadDefinition:
        return "Squad / group";
    case Role::triggerSource:
        return "Trigger / volume";
    case Role::unknown:
        return "Unknown";
    }
    return "Unknown";
}

const char* confidence_name(Confidence confidence) noexcept {
    switch (confidence) {
    case Confidence::unknown:
        return "unknown";
    case Confidence::probable:
        return "probable";
    case Confidence::strong:
        return "strong";
    }
    return "unknown";
}

} // namespace sunrise::client::inspection::activity_logic_catalog
