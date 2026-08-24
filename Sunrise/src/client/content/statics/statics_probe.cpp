#include "statics_probe.h"

#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/statics_reader.h"
#include "../items/packages/internal.h"

namespace sunrise::client::content::statics {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Statics collections examined per pass; each read is a real decompression. */
constexpr std::size_t kProbeTags = 8;
/** Followed resource references per collection. */
constexpr std::size_t kFollowTags = 4;
/** Transforms parsed per array; enough to infer structure and a footprint. */
constexpr std::size_t kProbeTransforms = 512;
/** Instance groups followed per collection in the end-to-end chain. */
constexpr std::size_t kProbeGroups = 16;
/** Instances placed per group; enough for an honest footprint sample. */
constexpr std::size_t kProbeGroupInstances = 32;
/** Distinct mesh resources read per collection. */
constexpr std::size_t kProbeMeshes = 8;
/** Extent boxes parsed per array. */
constexpr std::size_t kProbeExtents = 16;

enum class ProbeState : std::uint8_t {
    idle,
    running,
    finished,
};

ProbeState g_state{ProbeState::idle};

/** Reads one little-endian word for a raw dump line. */
[[nodiscard]] std::uint32_t raw_word(std::span<const std::byte> blob, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    if (offset + sizeof(value) <= blob.size()) {
        std::memcpy(&value, blob.data() + offset, sizeof(value));
    }
    return value;
}

/** Reads one little-endian float for a raw dump line. */
[[nodiscard]] float raw_float(std::span<const std::byte> blob, std::size_t offset) noexcept {
    float value = 0.0F;
    if (offset + sizeof(value) <= blob.size()) {
        std::memcpy(&value, blob.data() + offset, sizeof(value));
    }
    return value;
}

bool log_line(const char* format, ...) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    va_list arguments{};
    va_start(arguments, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return false;
    }
    core::log::write(core::log::Channel::middleware,
                     core::log::Level::info,
                     {line.data(), static_cast<std::size_t>(written)});
    return true;
}

/** Reports one resource's self-describing arrays and parses the known ones. */
void probe_resource(std::uint32_t tag,
                    std::uint32_t entryClass,
                    std::span<const std::byte> blob) noexcept {
    std::array<tables::StaticsArray, tables::kStaticsArrayCapacity> arrays{};
    const std::size_t found = tables::find_all_statics_arrays(blob, arrays);
    log_line("ev=statics_probe_resource tag=%08X size=%zu arrays=%zu", tag, blob.size(), found);
    // The mesh resources keep their per-mesh records inline ahead of the tag
    // array; whole-resource dumps pin the extents-vs-centre lanes. Keep it to a
    // few small resources so the log stays readable.
    static std::size_t dumpedMeshes = 0;
    if (entryClass == tables::kStaticsMeshExtentsClass && blob.size() <= 0x180
        && dumpedMeshes < 3) {
        ++dumpedMeshes;
        for (std::size_t offset = 0; offset + 16U <= blob.size() && offset < 0x120; offset += 16U) {
            log_line("ev=statics_probe_words tag=%08X at=0x%02zX "
                     "%08X(%.4g) %08X(%.4g) %08X(%.4g) %08X(%.4g)",
                     tag,
                     offset,
                     raw_word(blob, offset),
                     raw_float(blob, offset),
                     raw_word(blob, offset + 4U),
                     raw_float(blob, offset + 4U),
                     raw_word(blob, offset + 8U),
                     raw_float(blob, offset + 8U),
                     raw_word(blob, offset + 12U),
                     raw_float(blob, offset + 12U));
        }
    }
    for (std::size_t index = 0; index < found; ++index) {
        const tables::StaticsArray& array = arrays[index];
        log_line("ev=statics_probe_array tag=%08X class=%08X count=%llu data=%zu desc=%zu",
                 tag,
                 array.elementClass,
                 static_cast<unsigned long long>(array.count),
                 array.dataOffset,
                 array.descriptorOffset);
        // Probe-confirmed record classes: instance transforms live in parallel
        // arrays of 0x30 records (inline 808071A3 and external 80809673).
        if (array.elementClass == tables::kStaticsTransformClass
            || array.elementClass == tables::kStaticsTransformModernClass
            || array.elementClass == 0x80809673U || array.elementClass == 0x808071A3U) {
            std::array<tables::StaticsTransform, kProbeTransforms> transforms{};
            const tables::StaticsParse parsed =
                tables::parse_statics_transforms(blob, array, transforms);
            log_line("ev=statics_probe_parse tag=%08X kind=transform stride=%zu parsed=%zu "
                     "rejected=%zu valid=%d",
                     tag,
                     parsed.stride,
                     parsed.parsed,
                     parsed.rejected,
                     parsed.valid ? 1 : 0);
            if (parsed.valid) {
                const tables::StaticsAabb box{{transforms[0].translation[0] - 1.0F,
                                               transforms[0].translation[1] - 1.0F,
                                               transforms[0].translation[2] - 1.0F},
                                              {transforms[0].translation[0] + 1.0F,
                                               transforms[0].translation[1] + 1.0F,
                                               transforms[0].translation[2] + 1.0F}};
                tables::StaticsAabb footprint{};
                if (tables::union_statics_footprint(
                        box,
                        std::span<const tables::StaticsTransform>(transforms).first(parsed.parsed),
                        footprint)) {
                    log_line("ev=statics_probe_footprint tag=%08X "
                             "min=%.1f,%.1f,%.1f max=%.1f,%.1f,%.1f",
                             tag,
                             static_cast<double>(footprint.minimum[0]),
                             static_cast<double>(footprint.minimum[1]),
                             static_cast<double>(footprint.minimum[2]),
                             static_cast<double>(footprint.maximum[0]),
                             static_cast<double>(footprint.maximum[1]),
                             static_cast<double>(footprint.maximum[2]));
                }
            }
        } else if (array.elementClass == tables::kStaticsMeshExtentsClass
                   || array.elementClass == tables::kTerrainAabbClass
                   || array.elementClass == tables::kOcclusionBoundsEntryClass) {
            std::array<tables::StaticsAabb, kProbeExtents> boxes{};
            const tables::StaticsParse parsed = tables::parse_statics_extents(blob, array, boxes);
            log_line("ev=statics_probe_parse tag=%08X kind=extents encoding=%d stride=%zu "
                     "parsed=%zu rejected=%zu valid=%d",
                     tag,
                     static_cast<int>(parsed.kind),
                     parsed.stride,
                     parsed.parsed,
                     parsed.rejected,
                     parsed.valid ? 1 : 0);
            if (parsed.valid) {
                log_line("ev=statics_probe_extent_box tag=%08X "
                         "min=%.1f,%.1f,%.1f max=%.1f,%.1f,%.1f",
                         tag,
                         static_cast<double>(boxes[0].minimum[0]),
                         static_cast<double>(boxes[0].minimum[1]),
                         static_cast<double>(boxes[0].minimum[2]),
                         static_cast<double>(boxes[0].maximum[0]),
                         static_cast<double>(boxes[0].maximum[1]),
                         static_cast<double>(boxes[0].maximum[2]));
            }
        }
    }
}

struct CollectContext final {
    std::array<std::uint32_t, kProbeTags> tags{};
    std::size_t count{};
};

bool collect_tag(void* context, std::uint32_t tag) noexcept {
    auto* collection = static_cast<CollectContext*>(context);
    if (collection->count < collection->tags.size()) {
        collection->tags[collection->count++] = tag;
    }
    return true;
}

/** Class candidates whose presence pins the installed build's numbering era. */
struct CensusCandidate final {
    const char* name;
    std::uint32_t klass;
};
inline constexpr std::array<CensusCandidate, 15> kCensus{{
    {"component_known_good", 0x808099D6U},
    {"bubble_parent_arrivals", 0x80807DAEU},
    {"bubble_parent_modern", 0x8080891EU},
    {"map_table_modern", 0x80809883U},
    {"map_entry_modern", 0x80809885U},
    {"statics_collection_arrivals", tables::kStaticsCollectionClass},
    {"statics_instances_modern", 0x808093ADU},
    {"occlusion_bounds_array_modern", 0x808093B1U},
    {"occlusion_bounds_entry_modern", 0x808093B3U},
    {"statics_transform_arrivals", tables::kStaticsTransformClass},
    {"statics_transform_modern", 0x80806D40U},
    {"instance_group_modern", 0x80806D28U},
    {"mesh_extents_arrivals", tables::kStaticsMeshExtentsClass},
    {"terrain_arrivals", tables::kTerrainClass},
    {"terrain_modern", 0x80806C81U},
}};

bool count_matches(void* context, std::uint32_t) noexcept {
    ++*static_cast<std::size_t*>(context);
    return true;
}

/** Scans each candidate class and reports which era's numbering the install uses. */
std::uint32_t census_classes(const wchar_t* directory) noexcept {
    std::uint32_t probeClass = 0;
    for (const CensusCandidate& candidate : kCensus) {
        std::size_t count = 0;
        reader::ScanResult result{};
        if (reader::scan_class(directory, candidate.klass, &count_matches, &count, result)
            && count != 0) {
            log_line("ev=statics_probe_census class=%08X name=%s matches=%zu packages=%zu",
                     candidate.klass,
                     candidate.name,
                     count,
                     result.packages);
            // The transform-array and extent resources are their own top-level
            // entries in this install, so either is a directly probeable class.
            if (probeClass == 0
                && (candidate.klass == tables::kStaticsCollectionClass
                    || candidate.klass == 0x808093ADU
                    || candidate.klass == tables::kStaticsTransformClass
                    || candidate.klass == tables::kStaticsMeshExtentsClass)) {
                probeClass = candidate.klass;
            }
        }
    }
    return probeClass;
}

/** Follows up to a few resource handles embedded in one collection blob. */
void follow_references(const reader::Source& source,
                       reader::Scratch& scratch,
                       std::uint32_t tag,
                       std::span<const std::byte> blob) noexcept {
    std::array<std::uint32_t, kFollowTags> followed{};
    std::size_t followedCount = 0;
    for (std::size_t offset = 0;
         offset + sizeof(std::uint32_t) <= blob.size() && followedCount < kFollowTags;
         offset += sizeof(std::uint32_t)) {
        std::uint32_t candidate = 0;
        std::memcpy(&candidate, blob.data() + offset, sizeof(candidate));
        if (candidate == tag || tables::package_of(candidate) == tables::kAbsentPackageId) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t seen = 0; seen < followedCount; ++seen) {
            duplicate = duplicate || followed[seen] == candidate;
        }
        if (duplicate) {
            continue;
        }
        std::vector<std::byte> referenced{};
        std::uint32_t classId = 0;
        if (reader::read_tag(source, scratch, candidate, referenced, classId)) {
            followed[followedCount++] = candidate;
            log_line("ev=statics_probe_follow tag=%08X from=%08X class=%08X size=%zu",
                     candidate,
                     tag,
                     classId,
                     referenced.size());
            probe_resource(candidate, classId, referenced);
        }
    }
}

/**
 * Runs the probe-confirmed chain end to end for one collection: instance
 * transforms x instance groups x mesh resources, producing a real world AABB.
 */
void probe_collection_chain(const reader::Source& source,
                            reader::Scratch& scratch,
                            std::uint32_t tag,
                            std::span<const std::byte> blob) noexcept {
    std::array<tables::StaticsArray, tables::kStaticsArrayCapacity> arrays{};
    const std::size_t found = tables::find_all_statics_arrays(blob, arrays);
    const tables::StaticsArray* records = nullptr;
    const tables::StaticsArray* meshTags = nullptr;
    const tables::StaticsArray* groups = nullptr;
    for (std::size_t index = 0; index < found; ++index) {
        if (arrays[index].elementClass == tables::kStaticsInstanceRecordsClass) {
            records = &arrays[index];
        } else if (arrays[index].elementClass == tables::kStaticsMeshTagsClass) {
            meshTags = &arrays[index];
        } else if (arrays[index].elementClass == tables::kStaticsGroupsClass) {
            groups = &arrays[index];
        }
    }
    if (records == nullptr || meshTags == nullptr || groups == nullptr) {
        log_line("ev=statics_probe_chain tag=%08X result=incomplete records=%d tags=%d "
                 "groups=%d",
                 tag,
                 records != nullptr,
                 meshTags != nullptr,
                 groups != nullptr);
        return;
    }
    std::array<tables::StaticsTransform, kProbeTransforms> transforms{};
    const tables::StaticsParse parsed =
        tables::parse_statics_transforms(blob, *records, transforms);
    if (!parsed.valid) {
        log_line("ev=statics_probe_chain tag=%08X result=transforms_invalid", tag);
        return;
    }
    tables::StaticsAabb footprint{};
    footprint.minimum.fill(3.4e38F);
    footprint.maximum.fill(-3.4e38F);
    std::size_t placed = 0;
    std::size_t meshesRead = 0;
    std::size_t extentsRejected = 0;
    std::array<std::uint32_t, kProbeMeshes> readMeshes{};
    const std::size_t groupCount =
        (std::min)(static_cast<std::size_t>(groups->count), kProbeGroups);
    for (std::size_t group = 0; group < groupCount && meshesRead < kProbeMeshes; ++group) {
        tables::StaticsGroup row{};
        if (!tables::statics_group_at(blob, *groups, group, row)
            || row.meshIndex >= static_cast<std::size_t>(meshTags->count)) {
            continue;
        }
        std::uint32_t meshTag = 0;
        if (!tables::statics_mesh_tag_at(blob, *meshTags, row.meshIndex, meshTag)) {
            continue;
        }
        bool already = false;
        for (std::size_t seen = 0; seen < meshesRead; ++seen) {
            already = already || readMeshes[seen] == meshTag;
        }
        if (already) {
            continue;
        }
        std::vector<std::byte> mesh{};
        std::uint32_t meshClass = 0;
        if (!reader::read_tag(source, scratch, meshTag, mesh, meshClass)) {
            continue;
        }
        readMeshes[meshesRead++] = meshTag;
        tables::StaticsAabb local{};
        if (!tables::mesh_resource_extents(mesh, local)) {
            ++extentsRejected;
            continue;
        }
        const std::size_t instanceCount =
            (std::min)(static_cast<std::size_t>(row.instanceCount), kProbeGroupInstances);
        for (std::size_t instance = 0; instance < instanceCount; ++instance) {
            const std::size_t ordinal = row.instanceStart + instance;
            if (ordinal >= parsed.parsed) {
                break;
            }
            tables::StaticsAabb placedBox{};
            if (tables::transform_statics_aabb(local, transforms[ordinal], placedBox)) {
                for (std::size_t lane = 0; lane < 3; ++lane) {
                    footprint.minimum[lane] =
                        (std::min)(footprint.minimum[lane], placedBox.minimum[lane]);
                    footprint.maximum[lane] =
                        (std::max)(footprint.maximum[lane], placedBox.maximum[lane]);
                }
                ++placed;
            }
        }
    }
    if (placed == 0) {
        log_line("ev=statics_probe_chain tag=%08X result=no_placed groups=%zu "
                 "meshes=%zu rejected=%zu",
                 tag,
                 groupCount,
                 meshesRead,
                 extentsRejected);
        return;
    }
    log_line("ev=statics_probe_chain tag=%08X result=footprint groups=%zu meshes=%zu "
             "rejected=%zu placed=%zu min=%.1f,%.1f,%.1f max=%.1f,%.1f,%.1f",
             tag,
             groupCount,
             meshesRead,
             extentsRejected,
             placed,
             static_cast<double>(footprint.minimum[0]),
             static_cast<double>(footprint.minimum[1]),
             static_cast<double>(footprint.minimum[2]),
             static_cast<double>(footprint.maximum[0]),
             static_cast<double>(footprint.maximum[1]),
             static_cast<double>(footprint.maximum[2]));
}

} // namespace

void request_structure_probe() noexcept {
    if (g_state != ProbeState::idle) {
        return;
    }
    g_state = ProbeState::running;
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!items::packages::collect_keys(keys) || !items::packages::package_directory(directory)) {
        log_line("ev=statics_probe result=unavailable reason=keys_or_directory");
        g_state = ProbeState::finished;
        return;
    }
    (void)census_classes(directory.chars.data());
    static reader::Scratch scratch{};
    const reader::Source source{directory.chars.data(), &keys};
    std::size_t probed = 0;
    // Transform-array resources first: they are the instance placements, and the
    // extents resources are probed directly and through followed references.
    const std::array<std::uint32_t, 2> probeClasses{tables::kStaticsTransformClass,
                                                    tables::kStaticsMeshExtentsClass};
    for (const std::uint32_t probeClass : probeClasses) {
        CollectContext collection{};
        reader::ScanResult scan{};
        if (!reader::scan_class(
                directory.chars.data(), probeClass, &collect_tag, &collection, scan)) {
            log_line("ev=statics_probe result=failed reason=scan class=%08X", probeClass);
            continue;
        }
        log_line("ev=statics_probe result=collected class=%08X matches=%zu probing=%zu",
                 probeClass,
                 scan.matches,
                 collection.count);
        for (std::size_t index = 0; index < collection.count; ++index) {
            std::vector<std::byte> blob{};
            std::uint32_t classId = 0;
            if (!reader::read_tag(source, scratch, collection.tags[index], blob, classId)) {
                log_line("ev=statics_probe_resource tag=%08X result=read_failed",
                         collection.tags[index]);
                continue;
            }
            log_line("ev=statics_probe_resource tag=%08X entry_class=%08X",
                     collection.tags[index],
                     classId);
            probe_resource(collection.tags[index], classId, blob);
            if (probeClass == tables::kStaticsTransformClass) {
                probe_collection_chain(source, scratch, collection.tags[index], blob);
                follow_references(source, scratch, collection.tags[index], blob);
            }
            ++probed;
        }
    }
    SecureZeroMemory(&keys, sizeof keys);
    log_line("ev=statics_probe result=done probed=%zu", probed);
    g_state = ProbeState::finished;
}

bool structure_probe_running() noexcept {
    return g_state == ProbeState::running;
}

bool structure_probe_finished() noexcept {
    return g_state == ProbeState::finished;
}

} // namespace sunrise::client::content::statics
