#include "activity_logic_packages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <tuple>

#include "../../../middleware/content/packages/tables/internal.h"
#include "../../../middleware/content/packages/tables/scenario_walk.h"
#include "../../../middleware/gameplay/peer/join_messages.h"

namespace sunrise::client::content::activity::logic_packages {
namespace {

namespace catalog = inspection::activity_logic_catalog;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

constexpr std::uint32_t kActivityResourceClass = 0x80809462U;
constexpr std::uint32_t kPrimaryRowClass = 0x80809B3AU;
constexpr std::uint32_t kGroupRowClass = 0x80809464U;
constexpr std::uint32_t kChildRowClass = 0x80809466U;
constexpr std::uint32_t kWrapperClass = 0x80809468U;
constexpr std::uint32_t kEntityReferenceClass = 0x80809B14U;
constexpr std::uint32_t kEntityDefinitionClass = 0x80809C36U;
constexpr std::uint32_t kMapTableClass = 0x808099D6U;
constexpr std::uint32_t kMapRowClass = 0x808099D8U;
constexpr std::size_t kPrimaryDescriptor = 0x20;
constexpr std::size_t kPrimaryStride = 8;
constexpr std::size_t kGroupDescriptor = 0x38;
constexpr std::size_t kGroupStride = 0x18;
constexpr std::size_t kGroupChildrenDescriptor = 8;
constexpr std::size_t kChildStride = 4;
constexpr std::size_t kMapDescriptor = 8;
constexpr std::size_t kMapStride = 0x90;
constexpr std::size_t kMaximumRows = 250000;

struct Primary final {
    std::uint32_t discriminator{};
    std::uint32_t nameHash{};
};

struct Resource final {
    std::uint32_t tag{};
    std::vector<Primary> primary;
    std::vector<std::uint32_t> wrappers;
};

struct Target final {
    std::uint32_t resourceTag{};
    std::uint32_t tag{};
    std::uint32_t classId{};
};

struct Membership final {
    std::uint32_t resourceTag{};
    std::uint32_t definitionTag{};
    std::uint32_t entityIndex{};
};

struct MatchedNames final {
    std::string best;
    std::vector<std::uint32_t> hashes;
};

using PlacementsByWorldId =
    std::unordered_map<std::uint64_t, std::vector<catalog::Placement>>;

struct Build final {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    Cancelled cancelled{};
    void* cancelContext{};
    std::array<std::vector<std::byte>, static_cast<std::size_t>(tables::ReadSlot::count)> slots;
    std::vector<Resource> resources;
    std::unordered_set<std::uint32_t> resourceTags;
    std::size_t rejected{};
};

[[nodiscard]] bool stopped(const Build& build) noexcept {
    return build.cancelled != nullptr && build.cancelled(build.cancelContext);
}

[[nodiscard]] std::uint32_t fnv1(std::string_view text) noexcept {
    std::uint32_t value = 0x811C9DC5U;
    for (const unsigned char byte : text) {
        value = value * 0x01000193U ^ byte;
    }
    return value;
}

[[nodiscard]] bool array_at(std::span<const std::byte> blob,
                            std::size_t descriptor,
                            std::uint32_t elementClass,
                            std::size_t stride,
                            tables::Array& output) noexcept {
    if (!tables::find_array_at(blob, descriptor, output) || output.elementClass != elementClass
        || output.count > kMaximumRows || output.dataOffset > blob.size()
        || output.count > (blob.size() - output.dataOffset) / stride) {
        output = {};
        return false;
    }
    return true;
}

[[nodiscard]] bool read_expected(Build& build,
                                 tables::ReadSlot slot,
                                 std::uint32_t tag,
                                 std::uint32_t expected,
                                 std::span<const std::byte>& blob) noexcept {
    auto& bytes = build.slots[static_cast<std::size_t>(slot)];
    std::uint32_t classId = 0;
    bytes.clear();
    if (stopped(build) || !reader::read_tag(*build.source, *build.scratch, tag, bytes, classId)
        || classId != expected) {
        blob = {};
        return false;
    }
    blob = bytes;
    return true;
}

[[nodiscard]] bool scenario_read(void* context,
                                 tables::ReadSlot slot,
                                 std::uint32_t tag,
                                 std::span<const std::byte>& blob) noexcept {
    auto& build = *static_cast<Build*>(context);
    constexpr std::array<std::uint32_t, static_cast<std::size_t>(tables::ReadSlot::count)> classes = {
        tables::kSliceEntryClass,
        tables::kObjectRegistryClass,
        kActivityResourceClass,
    };
    return read_expected(build, slot, tag, classes[static_cast<std::size_t>(slot)], blob);
}

[[nodiscard]] bool collect_resource(void* context, const tables::Placement& placement) noexcept {
    auto& build = *static_cast<Build*>(context);
    if (stopped(build) || !build.resourceTags.insert(placement.objectTag).second) {
        return !stopped(build);
    }
    Resource resource{};
    resource.tag = placement.objectTag;
    tables::Array primary{};
    tables::Array groups{};
    if (!array_at(placement.objectBytes,
                  kPrimaryDescriptor,
                  kPrimaryRowClass,
                  kPrimaryStride,
                  primary)
        || !array_at(placement.objectBytes,
                     kGroupDescriptor,
                     kGroupRowClass,
                     kGroupStride,
                     groups)) {
        ++build.rejected;
        return true;
    }
    resource.primary.reserve(static_cast<std::size_t>(primary.count));
    for (std::uint64_t index = 0; index < primary.count; ++index) {
        std::size_t offset = 0;
        Primary row{};
        if (!tables::element_offset(primary.dataOffset, primary.count, kPrimaryStride, index, offset)
            || !tables::read(placement.objectBytes, offset, row.discriminator)
            || !tables::read(placement.objectBytes, offset + 4, row.nameHash)) {
            return false;
        }
        resource.primary.push_back(row);
    }
    for (std::uint64_t group = 0; group < groups.count; ++group) {
        std::size_t offset = 0;
        tables::Array children{};
        if (!tables::element_offset(groups.dataOffset, groups.count, kGroupStride, group, offset)
            || !array_at(placement.objectBytes,
                         offset + kGroupChildrenDescriptor,
                         kChildRowClass,
                         kChildStride,
                         children)) {
            return false;
        }
        for (std::uint64_t child = 0; child < children.count; ++child) {
            std::size_t childOffset = 0;
            std::uint32_t tag = 0;
            if (!tables::element_offset(
                    children.dataOffset, children.count, kChildStride, child, childOffset)
                || !tables::read(placement.objectBytes, childOffset, tag)) {
                return false;
            }
            resource.wrappers.push_back(tag);
        }
    }
    std::sort(resource.wrappers.begin(), resource.wrappers.end());
    resource.wrappers.erase(
        std::unique(resource.wrappers.begin(), resource.wrappers.end()), resource.wrappers.end());
    build.resources.push_back(std::move(resource));
    return true;
}

[[nodiscard]] bool wrapper_target(std::span<const std::byte> blob,
                                  std::uint32_t& tag) noexcept {
    tag = 0;
    const std::size_t offset = blob.size() == 32 ? 8 : blob.size() == 68 ? 0x40 : SIZE_MAX;
    return offset != SIZE_MAX && tables::read(blob, offset, tag)
           && tables::package_of(tag) != tables::kAbsentPackageId;
}

[[nodiscard]] std::pair<catalog::Role, catalog::Confidence>
role_for(std::uint32_t primary, std::uint32_t secondary) noexcept {
    using R = catalog::Role;
    using C = catalog::Confidence;
    const std::uint64_t pair = (static_cast<std::uint64_t>(primary) << 32U) | secondary;
    switch (pair) {
        case 0x808099C8808099C9ULL: return {R::triggerSource, C::strong};
        case 0x8080952280809523ULL: return {R::triggerSource, C::strong};
        case 0x80804ED080804ED1ULL: return {R::triggerSource, C::strong};
        case 0x808094EE808094EFULL: return {R::conditionMonitor, C::strong};
        case 0x8080956880809569ULL: return {R::conditionMonitor, C::probable};
        case 0x80804F0180804F02ULL: return {R::actionSequence, C::strong};
        case 0x808094CF808094D0ULL: return {R::spawnDefinition, C::strong};
        case 0x80809A3B8080948FULL: return {R::squadDefinition, C::strong};
        case 0x808083488080835AULL: return {R::objective, C::probable};
        case 0x80807D8780807D88ULL: return {R::objective, C::strong};
        case 0x8080992780809928ULL: return {R::object, C::strong};
        case 0x80804F4580804F46ULL: return {R::device, C::strong};
        case 0x8080834280807D97ULL: return {R::actionTarget, C::probable};
        case 0x8080834680807D98ULL: return {R::spatialRule, C::strong};
        case 0x808094D1808094D2ULL: return {R::actionTarget, C::strong};
        case 0x808094D7808094D8ULL: return {R::spatialRule, C::probable};
        case 0x80804F0680804F07ULL: return {R::actionSequence, C::strong};
        default: return {R::unknown, C::unknown};
    }
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
component_classes(std::span<const std::byte> blob) noexcept {
    std::array<std::uint32_t, 2> classes{};
    std::size_t count = 0;
    for (const std::size_t field : {std::size_t{8}, std::size_t{16}, std::size_t{24}}) {
        std::int64_t relative = 0;
        if (!tables::read(blob, field, relative) || relative == 0) {
            continue;
        }
        const std::int64_t target = static_cast<std::int64_t>(field) + relative;
        if (target < 4 || static_cast<std::uint64_t>(target) > blob.size()) {
            continue;
        }
        std::uint32_t classId = 0;
        if (!tables::read(blob, static_cast<std::size_t>(target) - 4, classId)
            || classId < tables::kTagLowerBound || classId >= tables::kTagUpperBound) {
            continue;
        }
        if (count < classes.size()) {
            classes[count++] = classId;
        }
    }
    return {classes[0], classes[1]};
}

[[nodiscard]] MatchedNames matched_names(std::span<const std::byte> blob,
                                         const Resource& resource) {
    MatchedNames result{};
    std::unordered_set<std::uint32_t> hashes;
    for (const Primary& row : resource.primary) {
        hashes.insert(row.nameHash);
    }
    std::string best;
    for (std::size_t offset = 0; offset < blob.size();) {
        const unsigned char first = static_cast<unsigned char>(blob[offset]);
        if (first < 0x20 || first > 0x7E) {
            ++offset;
            continue;
        }
        std::size_t end = offset;
        while (end < blob.size() && end - offset <= 192) {
            const unsigned char value = static_cast<unsigned char>(blob[end]);
            if (value == 0) {
                break;
            }
            if (value < 0x20 || value > 0x7E) {
                end = offset;
                break;
            }
            ++end;
        }
        if (end > offset + 2 && end < blob.size() && blob[end] == std::byte{0}) {
            const std::string_view text(reinterpret_cast<const char*>(blob.data() + offset),
                                        end - offset);
            const std::uint32_t hash = fnv1(text);
            if (hashes.contains(hash)) {
                if (std::ranges::find(result.hashes, hash) == result.hashes.end()) {
                    result.hashes.push_back(hash);
                }
                if (text.size() > result.best.size()) {
                    result.best.assign(text);
                }
            }
            offset = end + 1;
        } else {
            ++offset;
        }
    }
    return result;
}

[[nodiscard]] bool secondary_span(std::span<const std::byte> blob,
                                  std::uint32_t expectedClass,
                                  std::size_t& begin,
                                  std::size_t& end) noexcept {
    constexpr std::size_t kStartField = 0x18;
    constexpr std::size_t kEndField = 0x28;
    std::int64_t startRelative = 0;
    std::int64_t endRelative = 0;
    if (!tables::read(blob, kStartField, startRelative)
        || !tables::read(blob, kEndField, endRelative) || startRelative <= 0) {
        return false;
    }
    const std::int64_t start = static_cast<std::int64_t>(kStartField) + startRelative;
    const std::int64_t finish = static_cast<std::int64_t>(kEndField) + endRelative + 0x10;
    if (start < 4 || finish < start || static_cast<std::uint64_t>(finish) > blob.size()) {
        return false;
    }
    std::uint32_t classId = 0;
    if (!tables::read(blob, static_cast<std::size_t>(start) - 4U, classId)
        || classId != expectedClass) {
        return false;
    }
    begin = static_cast<std::size_t>(start);
    end = static_cast<std::size_t>(finish);
    return true;
}

[[nodiscard]] bool finite_placement(const catalog::Placement& placement) noexcept {
    return std::ranges::all_of(placement.position, [](float value) { return std::isfinite(value); })
           && std::ranges::all_of(placement.rotation,
                                  [](float value) { return std::isfinite(value); });
}

/**
 * Resolves authored placement references embedded in one serialized entity definition.
 * Map rows name the placed class, while logic definitions refer to the stable 64-bit world id.
 * The serialized fields are naturally 32-bit aligned; requiring that alignment also prevents
 * matching byte-shifted substrings inside unrelated scalar values.
 */
[[nodiscard]] std::size_t
append_world_placements(std::span<const std::byte> blob,
                        const PlacementsByWorldId& placementsByWorldId,
                        std::vector<catalog::Placement>& output) {
    if (blob.size() < sizeof(std::uint64_t) || placementsByWorldId.empty()) {
        return 0;
    }
    const std::size_t originalSize = output.size();
    std::unordered_set<std::uint64_t> matched;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= blob.size(); offset += 4U) {
        std::uint64_t worldId = 0;
        if (!tables::read(blob, offset, worldId)) {
            continue;
        }
        const auto found = placementsByWorldId.find(worldId);
        if (found == placementsByWorldId.end() || !matched.insert(worldId).second) {
            continue;
        }
        output.insert(output.end(), found->second.begin(), found->second.end());
    }
    return output.size() - originalSize;
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
        if (scenarioTag == 0 || mapFamily.empty() || contentFingerprint.size() != 32) {
            return false;
        }
        Build build{&source, &scratch, cancelled, cancelContext};
        std::vector<std::byte> scenario;
        std::uint32_t scenarioClass = 0;
        if (!reader::read_tag(source, scratch, scenarioTag, scenario, scenarioClass)
            || scenarioClass != tables::kScenarioClass) {
            return false;
        }
        tables::WalkResult walked{};
        if (!tables::walk_scenario(
                scenario, &scenario_read, &build, &collect_resource, &build, walked)
            || stopped(build)) {
            return false;
        }
        std::sort(build.resources.begin(), build.resources.end(), [](const auto& a, const auto& b) {
            return a.tag < b.tag;
        });
        progress.resources = build.resources.size();
        progress.rejected = build.rejected;

        std::vector<Target> targets;
        std::unordered_set<std::uint64_t> targetKeys;
        std::vector<std::byte> blob;
        for (const Resource& resource : build.resources) {
            for (const std::uint32_t wrapper : resource.wrappers) {
                if (stopped(build)) {
                    return false;
                }
                std::uint32_t classId = 0;
                blob.clear();
                std::uint32_t targetTag = 0;
                if (!reader::read_tag(source, scratch, wrapper, blob, classId)
                    || classId != kWrapperClass || !wrapper_target(blob, targetTag)) {
                    ++progress.rejected;
                    continue;
                }
                blob.clear();
                if (!reader::read_tag(source, scratch, targetTag, blob, classId)
                    || (classId != kEntityReferenceClass && classId != kMapTableClass)) {
                    ++progress.rejected;
                    continue;
                }
                const std::uint64_t key = (static_cast<std::uint64_t>(resource.tag) << 32U) | targetTag;
                if (targetKeys.insert(key).second) {
                    targets.push_back({resource.tag, targetTag, classId});
                }
            }
        }

        std::unordered_map<std::uint32_t, const Resource*> resources;
        for (const Resource& resource : build.resources) {
            resources.emplace(resource.tag, &resource);
        }
        PlacementsByWorldId placementsByWorldId;
        std::unordered_set<std::uint32_t> mapTags;
        for (const Target& target : targets) {
            if (target.classId != kMapTableClass || !mapTags.insert(target.tag).second) {
                continue;
            }
            std::uint32_t classId = 0;
            blob.clear();
            tables::Array rows{};
            if (!reader::read_tag(source, scratch, target.tag, blob, classId)
                || classId != kMapTableClass
                || !array_at(blob, kMapDescriptor, kMapRowClass, kMapStride, rows)) {
                ++progress.rejected;
                continue;
            }
            for (std::uint64_t index = 0; index < rows.count; ++index) {
                std::size_t offset = 0;
                catalog::Placement placement{};
                placement.mapTableTag = target.tag;
                if (!tables::element_offset(rows.dataOffset, rows.count, kMapStride, index, offset)
                    || !tables::read(blob, offset, placement.placedEntityTag)
                    || !tables::read(blob, offset + 0x70, placement.worldId)
                    || !tables::read(blob, offset + 0x20, placement.position)
                    || !tables::read(blob, offset + 0x10, placement.rotation)
                    || placement.worldId == 0 || !finite_placement(placement)) {
                    ++progress.rejected;
                    continue;
                }
                placementsByWorldId[placement.worldId].push_back(placement);
                ++progress.mapRows;
            }
        }

        std::unordered_map<std::uint32_t, std::uint32_t> entityIndex;
        std::unordered_map<std::uint32_t,
                           std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>>
            resourceNames;
        std::vector<Membership> memberships;
        std::unordered_set<std::uint64_t> membershipKeys;
        for (const Target& target : targets) {
            if (target.classId != kEntityReferenceClass || stopped(build)) {
                continue;
            }
            std::uint32_t classId = 0;
            std::uint32_t definitionTag = 0;
            std::uint32_t repeatedTag = 0;
            blob.clear();
            if (!reader::read_tag(source, scratch, target.tag, blob, classId)
                || classId != kEntityReferenceClass || !tables::read(blob, 0x0C, definitionTag)
                || !tables::read(blob, 0x10, repeatedTag) || definitionTag != repeatedTag) {
                ++progress.rejected;
                continue;
            }
            blob.clear();
            if (!reader::read_tag(source, scratch, definitionTag, blob, classId)
                || classId != kEntityDefinitionClass) {
                ++progress.rejected;
                continue;
            }
            const auto classes = component_classes(blob);
            if (classes.first == 0 || classes.second == 0) {
                ++progress.rejected;
                continue;
            }
            const Resource& resource = *resources.at(target.resourceTag);
            MatchedNames names = matched_names(blob, resource);
            auto existing = entityIndex.find(definitionTag);
            std::uint32_t index = 0;
            if (existing == entityIndex.end()) {
                catalog::Entity entity{};
                entity.definitionTag = definitionTag;
                entity.classPrimary = classes.first;
                entity.classSecondary = classes.second;
                std::tie(entity.role, entity.confidence) = role_for(classes.first, classes.second);
                entity.name = names.best;
                entity.label = catalog::role_name(entity.role);
                progress.publishedPlacements +=
                    append_world_placements(blob, placementsByWorldId, entity.placements);
                index = static_cast<std::uint32_t>(output.entities.size());
                entityIndex.emplace(definitionTag, index);
                output.entities.push_back(std::move(entity));
            } else {
                index = existing->second;
                if (names.best.size() > output.entities[index].name.size()) {
                    output.entities[index].name = names.best;
                }
            }
            const std::uint64_t membershipKey =
                (static_cast<std::uint64_t>(target.resourceTag) << 32U) | definitionTag;
            if (membershipKeys.insert(membershipKey).second) {
                memberships.push_back({target.resourceTag, definitionTag, index});
            }
            for (const std::uint32_t hash : names.hashes) {
                auto& owners = resourceNames[target.resourceTag][hash];
                if (std::ranges::find(owners, index) == owners.end()) {
                    owners.push_back(index);
                }
            }
        }

        using OccurrenceKey =
            std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::size_t>;
        using EdgeKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
        std::set<OccurrenceKey> occurrences;
        std::map<EdgeKey, std::uint32_t> edgeCounts;
        for (const Membership& membership : memberships) {
            if (stopped(build)) {
                return false;
            }
            std::uint32_t classId = 0;
            blob.clear();
            if (!reader::read_tag(source, scratch, membership.definitionTag, blob, classId)
                || classId != kEntityDefinitionClass) {
                ++progress.rejected;
                continue;
            }
            std::size_t begin = 0;
            std::size_t end = 0;
            if (!secondary_span(
                    blob, output.entities[membership.entityIndex].classSecondary, begin, end)) {
                ++progress.rejected;
                continue;
            }
            const auto resource = resourceNames.find(membership.resourceTag);
            if (resource == resourceNames.end()) {
                continue;
            }
            for (std::size_t offset = begin; offset <= end && end - offset >= sizeof(std::uint32_t);
                 offset += sizeof(std::uint32_t)) {
                std::uint32_t hash = 0;
                if (!tables::read(blob, offset, hash)) {
                    return false;
                }
                const auto targetsForName = resource->second.find(hash);
                if (targetsForName == resource->second.end()) {
                    continue;
                }
                for (const std::uint32_t targetIndex : targetsForName->second) {
                    if (targetIndex == membership.entityIndex) {
                        continue;
                    }
                    const OccurrenceKey occurrence{membership.definitionTag,
                                                   output.entities[targetIndex].definitionTag,
                                                   hash,
                                                   offset};
                    if (occurrences.insert(occurrence).second) {
                        ++edgeCounts[{membership.entityIndex, targetIndex, hash}];
                    }
                }
            }
        }
        output.edges.reserve(edgeCounts.size());
        for (const auto& [key, count] : edgeCounts) {
            output.edges.push_back(
                {std::get<0>(key), std::get<1>(key), std::get<2>(key), count});
        }

        catalog::Activity activity{};
        activity.scenarioTag = scenarioTag;
        activity.name = mapFamily;
        activity.destination = mapFamily;
        activity.entityIndices.resize(output.entities.size());
        for (std::size_t index = 0; index < output.entities.size(); ++index) {
            activity.entityIndices[index] = static_cast<std::uint32_t>(index);
        }
        output.activities.push_back(std::move(activity));
        output.provenance.collectorVersion = catalog::kCollectorVersion;
        output.provenance.contentBuild = middleware::gameplay::peer::kHostBuild;
        std::transform(contentFingerprint.begin(),
                       contentFingerprint.end(),
                       output.provenance.contentFingerprint.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
        progress.definitions = output.entities.size();
        progress.references = output.edges.size();
        std::string error;
        return !output.entities.empty() && catalog::validate(output, error);
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::logic_packages
