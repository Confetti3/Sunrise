#include "bubble_bounds_packages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../middleware/content/packages/tables/internal.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/statics_reader.h"
#include "../../../middleware/gameplay/peer/join_messages.h"

namespace sunrise::client::content::bubbles::packages {
namespace {

namespace catalog = inspection::bubble_catalog;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

constexpr std::uint32_t kParentClass = 0x80807DAEU;
constexpr std::uint32_t kDefinitionClass = 0x808091E0U;
constexpr std::uint32_t kDefinitionResourceClass = 0x808084C1U;
constexpr std::uint32_t kContainerClass = 0x80808A54U;
constexpr std::uint32_t kContainerTableClass = 0x80808BB0U;
constexpr std::uint32_t kTableClass = 0x808099D6U;
constexpr std::uint32_t kTableEntryClass = 0x808099D8U;
constexpr std::uint32_t kResourceClass = tables::kStaticsCollectionClass;
constexpr std::uint32_t kPreheaderClass = tables::kStaticsResolutionClass;
constexpr std::uint32_t kCollectionClass = tables::kStaticsTransformClass;
constexpr std::size_t kMaximumRows = 4096;
constexpr std::uint64_t kMaximumArray = 300000;
constexpr float kIdentityEpsilon = 0.000001F;

struct ContainerRef final {
    std::uint32_t tag{};
    std::uint64_t hash{};
};

struct PendingBubble final {
    std::uint32_t parentTag{};
    std::uint32_t nameHash{};
    std::vector<ContainerRef> containers;
};

struct ParentScan final {
    std::array<std::uint32_t, kMaximumRows> tags{};
    std::size_t count{};
    bool overflow{};
};

[[nodiscard]] bool stopped(Cancelled cancelled, void* context) noexcept {
    return cancelled != nullptr && cancelled(context);
}

[[nodiscard]] bool fail(Progress& progress, std::string message) {
    ++progress.rejected;
    progress.diagnostic = std::move(message);
    return false;
}

[[nodiscard]] bool collect_parent(void* context, std::uint32_t tag) noexcept {
    auto& scan = *static_cast<ParentScan*>(context);
    if (scan.count >= scan.tags.size()) {
        scan.overflow = true;
        return false;
    }
    scan.tags[scan.count++] = tag;
    return true;
}

[[nodiscard]] bool checked_relative(std::size_t field,
                                    std::int64_t relative,
                                    std::size_t size,
                                    std::size_t& target) noexcept {
    if (field > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    const std::int64_t base = static_cast<std::int64_t>(field);
    if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
        || (relative < 0 && base < (std::numeric_limits<std::int64_t>::min)() - relative)) {
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
    return count <= kMaximumArray && tables::find_array_at(blob, descriptor, output)
           && output.elementClass == elementClass && output.count == count
           && output.dataOffset <= blob.size()
           && count <= (blob.size() - output.dataOffset) / stride;
}

[[nodiscard]] bool read_expected(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 std::uint32_t tag,
                                 std::uint32_t expectedClass,
                                 std::vector<std::byte>& bytes) noexcept {
    bytes.clear();
    std::uint32_t classId = 0;
    return tables::package_of(tag) != tables::kAbsentPackageId
           && reader::read_tag(source, scratch, tag, bytes, classId)
           && classId == expectedClass;
}

[[nodiscard]] bool identity_entry(std::span<const std::byte> table,
                                  std::size_t row) noexcept {
    constexpr std::array<float, 4> identity{0.0F, 0.0F, 0.0F, 1.0F};
    for (const std::size_t offset : {std::size_t{0x10}, std::size_t{0x20}}) {
        for (std::size_t lane = 0; lane < identity.size(); ++lane) {
            float value = 0.0F;
            if (!tables::read(table, row + offset + lane * sizeof(float), value)
                || !std::isfinite(value)
                || std::fabs(value - identity[lane]) > kIdentityEpsilon) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool known_empty_dependency(std::string_view contentFamily,
                                          std::uint64_t hash) noexcept {
    // These two Mercury references have no row in any build-86657 misc hash table. The preserved
    // package chain resolves both to 81A6D710: one table with zero entries and no statics resource.
    // Keep the exception exact; ignoring an arbitrary unresolved dependency could under-bound it.
    return contentFamily == "mercury_destination"
           && (hash == 0x330FEF8E3BEE9513ULL || hash == 0x0136383885D2781AULL);
}

[[nodiscard]] bool collection_bounds(const reader::Source& source,
                                     reader::Scratch& scratch,
                                     std::uint32_t tag,
                                     std::array<float, 3>& minimum,
                                     std::array<float, 3>& maximum,
                                     std::size_t& placements,
                                     Cancelled cancelled,
                                     void* cancelContext) noexcept {
    std::vector<std::byte> blob;
    if (!read_expected(source, scratch, tag, kCollectionClass, blob)) {
        return false;
    }
    tables::Array transforms{};
    tables::Array groups{};
    if (!array_at(blob, 0x40, tables::kStaticsInstanceRecordsClass, 0x30, transforms)
        || !array_at(blob, 0x68, tables::kStaticsGroupsClass, 8, groups)
        || transforms.count == 0 || groups.count == 0) {
        return false;
    }
    const tables::StaticsArray groupArray{
        groups.elementClass, groups.count, groups.dataOffset, 0x68};
    bool found = false;
    for (std::size_t groupIndex = 0; groupIndex < groups.count; ++groupIndex) {
        if (stopped(cancelled, cancelContext)) {
            return false;
        }
        tables::StaticsGroup group{};
        if (!tables::statics_group_at(blob, groupArray, groupIndex, group)
            || group.instanceStart > transforms.count
            || group.instanceCount > transforms.count - group.instanceStart) {
            return false;
        }
        for (std::size_t ordinal = 0; ordinal < group.instanceCount; ++ordinal) {
            if ((ordinal & 0xFFU) == 0 && stopped(cancelled, cancelContext)) {
                return false;
            }
            const std::size_t transform = transforms.dataOffset
                                          + (static_cast<std::size_t>(group.instanceStart)
                                             + ordinal)
                                                * 0x30U;
            std::array<float, 3> point{};
            for (std::size_t lane = 0; lane < point.size(); ++lane) {
                if (!tables::read(blob, transform + 0x10U + lane * sizeof(float), point[lane])
                    || !std::isfinite(point[lane])) {
                    return false;
                }
                minimum[lane] = (std::min)(minimum[lane], point[lane]);
                maximum[lane] = (std::max)(maximum[lane], point[lane]);
            }
            found = true;
            ++placements;
        }
    }
    return found;
}

[[nodiscard]] bool table_bounds(const reader::Source& source,
                                reader::Scratch& scratch,
                                std::uint32_t tableTag,
                                std::unordered_set<std::uint32_t>& collections,
                                std::array<float, 3>& minimum,
                                std::array<float, 3>& maximum,
                                std::size_t& placements,
                                Cancelled cancelled,
                                void* cancelContext) noexcept {
    std::vector<std::byte> table;
    if (!read_expected(source, scratch, tableTag, kTableClass, table)) {
        return false;
    }
    tables::Array entries{};
    if (!array_at(table, 8, kTableEntryClass, 0x90, entries)) {
        return false;
    }
    for (std::size_t index = 0; index < entries.count; ++index) {
        if (stopped(cancelled, cancelContext)) {
            return false;
        }
        const std::size_t row = entries.dataOffset + index * 0x90U;
        const std::size_t pointer = row + 0x78U;
        std::int64_t relative = 0;
        if (!tables::read(table, pointer, relative)) {
            return false;
        }
        if (relative == 0 || relative == -1
            || relative == (std::numeric_limits<std::int64_t>::max)()) {
            continue;
        }
        std::size_t target = 0;
        std::uint32_t resourceClass = 0;
        if (!checked_relative(pointer, relative, table.size(), target) || target < 4
            || !tables::read(table, target - 4U, resourceClass)) {
            return false;
        }
        if (resourceClass != kResourceClass) {
            continue;
        }
        if (!identity_entry(table, row)) {
            return false;
        }
        std::uint32_t preheaderTag = 0;
        if (!tables::read(table, target + 0x10U, preheaderTag)) {
            return false;
        }
        std::vector<std::byte> preheader;
        if (!read_expected(source, scratch, preheaderTag, kPreheaderClass, preheader)) {
            return false;
        }
        std::uint32_t collectionTag = 0;
        if (!tables::read(preheader, 8, collectionTag)) {
            return false;
        }
        if (collections.insert(collectionTag).second
            && !collection_bounds(
                source,
                scratch,
                collectionTag,
                minimum,
                maximum,
                placements,
                cancelled,
                cancelContext)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool bubble_bounds(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 const PendingBubble& bubble,
                                 catalog::Bubble& output,
                                 std::size_t& placements,
                                 Cancelled cancelled,
                                 void* cancelContext) noexcept {
    std::unordered_set<std::uint32_t> containers;
    std::unordered_set<std::uint32_t> tablesSeen;
    std::unordered_set<std::uint32_t> collections;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    minimum.fill((std::numeric_limits<float>::max)());
    maximum.fill(-(std::numeric_limits<float>::max)());
    placements = 0;
    for (const ContainerRef& reference : bubble.containers) {
        if (stopped(cancelled, cancelContext)) {
            return false;
        }
        if (!containers.insert(reference.tag).second) {
            continue;
        }
        std::vector<std::byte> container;
        if (!read_expected(source, scratch, reference.tag, kContainerClass, container)) {
            return false;
        }
        tables::Array tableTags{};
        if (!array_at(container, 0x28, kContainerTableClass, 4, tableTags)) {
            return false;
        }
        for (std::size_t index = 0; index < tableTags.count; ++index) {
            if (stopped(cancelled, cancelContext)) {
                return false;
            }
            std::uint32_t tableTag = 0;
            if (!tables::read(container, tableTags.dataOffset + index * 4U, tableTag)) {
                return false;
            }
            if (tablesSeen.insert(tableTag).second
                && !table_bounds(source,
                                 scratch,
                                 tableTag,
                                 collections,
                                 minimum,
                                 maximum,
                                 placements,
                                 cancelled,
                                 cancelContext)) {
                return false;
            }
        }
    }
    if (placements == 0) {
        return false;
    }
    output.tag = bubble.parentTag;
    output.minimum = minimum;
    output.maximum = maximum;
    return true;
}

} // namespace

bool build(const reader::Source& source,
           reader::Scratch& scratch,
           std::uint32_t scenarioTag,
           std::string_view mapFamily,
           Cancelled cancelled,
           void* cancelContext,
           catalog::Catalog& output,
           Progress& progress) noexcept {
    output = {};
    progress = {};
    try {
        if (scenarioTag == 0 || mapFamily.empty() || source.directory.empty()
            || source.keys == nullptr) {
            return fail(progress, "Bubble bounds have no complete package scope.");
        }
        std::string contentFamily;
        if (!reader::content_family(source.directory,
                                    tables::package_of(scenarioTag),
                                    contentFamily)) {
            return fail(progress, "The scenario package name does not prove a content family.");
        }
        std::vector<std::byte> scenario;
        if (!read_expected(source, scratch, scenarioTag, tables::kScenarioClass, scenario)) {
            return fail(progress, "The current scenario package could not be read.");
        }
        tables::Array bubbles{};
        if (!tables::scenario_bubbles(scenario, bubbles)
            || bubbles.elementClass != tables::kBubbleClass || bubbles.count == 0
            || bubbles.count > kMaximumRows) {
            return fail(progress, "The current scenario bubble array is invalid.");
        }
        std::vector<std::uint32_t> bubbleHashes;
        bubbleHashes.reserve(static_cast<std::size_t>(bubbles.count));
        for (std::uint64_t index = 0; index < bubbles.count; ++index) {
            tables::Bubble bubble{};
            if (!tables::bubble_at(scenario, bubbles, index, bubble) || bubble.nameHash == 0
                || std::ranges::find(bubbleHashes, bubble.nameHash) != bubbleHashes.end()) {
                return fail(progress, "The current scenario has an invalid bubble identity.");
            }
            bubbleHashes.push_back(bubble.nameHash);
        }
        progress.scenarioBubbles = bubbleHashes.size();
        ParentScan parentScan{};
        reader::ScanResult scan{};
        if (!reader::scan_class_family(source.directory,
                                       contentFamily,
                                       kParentClass,
                                       &collect_parent,
                                       &parentScan,
                                       scan)
            || parentScan.overflow) {
            return fail(progress, "The bounded content-family bubble-parent scan failed.");
        }
        std::vector<PendingBubble> pending;
        std::vector<reader::Hash64Value> hashes;
        for (std::size_t index = 0; index < parentScan.count; ++index) {
            if (stopped(cancelled, cancelContext)) {
                progress.diagnostic = "Bubble-bounds package collection cancelled.";
                return false;
            }
            std::vector<std::byte> parent;
            if (!read_expected(source, scratch, parentScan.tags[index], kParentClass, parent)) {
                continue;
            }
            std::uint32_t definitionTag = 0;
            std::uint32_t nameHash = 0;
            if (!tables::read(parent, 8, definitionTag)
                || !tables::read(parent, 0x18, nameHash)
                || std::ranges::find(bubbleHashes, nameHash) == bubbleHashes.end()) {
                continue;
            }
            if (std::ranges::any_of(pending, [nameHash](const PendingBubble& value) {
                    return value.nameHash == nameHash;
                })) {
                return fail(progress, "A scenario bubble has duplicate package parents.");
            }
            std::vector<std::byte> definition;
            tables::Array resources{};
            if (!read_expected(source, scratch, definitionTag, kDefinitionClass, definition)
                || !array_at(definition, 8, kDefinitionResourceClass, 0x10, resources)) {
                ++progress.rejected;
                continue;
            }
            PendingBubble bubble{parentScan.tags[index], nameHash, {}};
            for (std::size_t row = 0; row < resources.count; ++row) {
                if ((row & 0xFFU) == 0 && stopped(cancelled, cancelContext)) {
                    progress.diagnostic = "Bubble-bounds package collection cancelled.";
                    return false;
                }
                const std::size_t at = resources.dataOffset + row * 0x10U;
                ContainerRef reference{};
                std::uint32_t flag = 0;
                if (!tables::read(definition, at, reference.tag)
                    || !tables::read(definition, at + 4U, flag)
                    || !tables::read(definition, at + 8U, reference.hash)) {
                    bubble.containers.clear();
                    break;
                }
                if (reference.tag == UINT32_MAX && flag == 0 && reference.hash != 0) {
                    reference.tag = 0;
                    if (known_empty_dependency(contentFamily, reference.hash)) {
                        ++progress.emptyDependencies;
                        continue;
                    }
                    if (std::ranges::none_of(hashes, [&](const reader::Hash64Value& value) {
                            return value.hash == reference.hash;
                        })) {
                        hashes.push_back(reader::Hash64Value{reference.hash});
                    }
                } else if (tables::package_of(reference.tag) == tables::kAbsentPackageId
                           || flag != 1 || reference.hash != 0) {
                    bubble.containers.clear();
                    break;
                }
                bubble.containers.push_back(reference);
            }
            if (!bubble.containers.empty()) {
                pending.push_back(std::move(bubble));
            } else {
                ++progress.rejected;
            }
        }
        progress.parents = pending.size();
        if (pending.empty()) {
            return fail(progress, "No package bubble parent matches the current scenario.");
        }
        if (!hashes.empty()) {
            reader::Hash64ScanResult hashScan{};
            if (!reader::resolve_hash64_scoped(
                    source.directory,
                    contentFamily,
                    hashes,
                    hashScan,
                    cancelled,
                    cancelContext)) {
                if (stopped(cancelled, cancelContext)) {
                    progress.diagnostic = "Bubble-bounds package collection cancelled.";
                    return false;
                }
                return fail(progress, "The scoped bubble hash table scan failed validation.");
            }
            progress.hashPackages = hashScan.packages;
            std::erase_if(pending, [&](PendingBubble& bubble) {
                bool invalid = false;
                for (ContainerRef& reference : bubble.containers) {
                    if (reference.tag != 0) {
                        continue;
                    }
                    const auto found = std::ranges::find_if(hashes, [&](const auto& value) {
                        return value.hash == reference.hash;
                    });
                    if (found == hashes.end() || !found->resolved
                        || found->classId != kContainerClass) {
                        invalid = true;
                        break;
                    }
                    reference.tag = found->tag;
                }
                if (invalid) {
                    ++progress.rejected;
                }
                return invalid;
            });
            if (pending.empty()) {
                return fail(progress, "No bubble container hashes resolved within package scope.");
            }
        }
        output.contentBuild = middleware::gameplay::peer::kHostBuild;
        output.family.assign(mapFamily);
        output.bubbles.reserve(pending.size());
        for (const PendingBubble& pendingBubble : pending) {
            if (stopped(cancelled, cancelContext)) {
                output = {};
                progress.diagnostic = "Bubble-bounds package collection cancelled.";
                return false;
            }
            catalog::Bubble bubble{};
            std::size_t placements = 0;
            if (!bubble_bounds(source,
                               scratch,
                               pendingBubble,
                               bubble,
                               placements,
                               cancelled,
                               cancelContext)) {
                if (stopped(cancelled, cancelContext)) {
                    output = {};
                    progress.diagnostic = "Bubble-bounds package collection cancelled.";
                    return false;
                }
                ++progress.rejected;
                continue;
            }
            bubble.family = output.family;
            output.bubbles.push_back(std::move(bubble));
            progress.placements += placements;
        }
        progress.published = output.bubbles.size();
        std::sort(output.bubbles.begin(), output.bubbles.end(), [](const auto& left, const auto& right) {
            return left.tag < right.tag;
        });
        std::string error;
        if (!catalog::validate(output, error)) {
            output = {};
            return fail(progress, "The package bubble bounds failed typed validation: " + error);
        }
        progress.diagnostic = "Package-native bubble bounds built for the current location.";
        return true;
    } catch (...) {
        output = {};
        progress.diagnostic = "Bubble-bounds package collection could not allocate scratch data.";
        return false;
    }
}

} // namespace sunrise::client::content::bubbles::packages
