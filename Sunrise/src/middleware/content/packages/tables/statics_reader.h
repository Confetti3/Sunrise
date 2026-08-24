#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * Arrivals-build statics classes, from the package-geometry evidence chain
 * (808071B3 collections resolved through 80806EF4 to 8080966D transforms and
 * 808071A7 mesh extents; terrain 8080714B resolved to 8080714F AABBs).
 */
inline constexpr std::uint32_t kStaticsCollectionClass = 0x808071B3U;
inline constexpr std::uint32_t kStaticsResolutionClass = 0x80806EF4U;
inline constexpr std::uint32_t kStaticsTransformClass = 0x8080966DU;
inline constexpr std::uint32_t kStaticsMeshExtentsClass = 0x808071A7U;
inline constexpr std::uint32_t kTerrainClass = 0x8080714BU;
inline constexpr std::uint32_t kTerrainAabbClass = 0x8080714FU;
/** Later-build numbering of the same resource family, for era-tolerant probes. */
inline constexpr std::uint32_t kStaticsInstancesModernClass = 0x808093ADU;
inline constexpr std::uint32_t kStaticsTransformModernClass = 0x80806D40U;
inline constexpr std::uint32_t kOcclusionBoundsEntryClass = 0x808093B3U;
inline constexpr std::uint32_t kTerrainModernClass = 0x80806C81U;

/**
 * Probe-confirmed array classes inside one 8080966D collection (2026-08-21 run):
 * instance transforms (0x30 records, quaternion/translation/scale), per-mesh
 * instance groups, and the mesh tag list naming 808071A7 resources.
 */
inline constexpr std::uint32_t kStaticsInstanceRecordsClass = 0x808071A3U;
inline constexpr std::uint32_t kStaticsGroupsClass = 0x80807190U;
inline constexpr std::uint32_t kStaticsMeshTagsClass = 0x8080967DU;
/** A mesh resource keeps its model half-extents at this fixed offset. */
inline constexpr std::size_t kMeshExtentsOffset = 0x38;
/** Half-extents beyond this are not a mesh box (probe-rejected garbage variant). */
inline constexpr float kMeshExtentLimit = 5000.0F;

/** How many self-describing arrays one pass reports per blob. */
inline constexpr std::size_t kStaticsArrayCapacity = 24;
/** Instance records longer than this are not transforms this build uses. */
inline constexpr std::size_t kMaximumTransformStride = 0x40;

/** One self-describing array located inside a statics blob. */
struct StaticsArray {
    std::uint32_t elementClass{};
    std::uint64_t count{};
    /** Offset of the first element inside the blob. */
    std::size_t dataOffset{};
    /** Offset of the descriptor that named the array. */
    std::size_t descriptorOffset{};
};

/** One instance placement: quaternion, translation, uniform scale. */
struct StaticsTransform {
    std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> translation{};
    float scale{1.0F};
};

/** One axis-aligned box in whatever space the source array uses. */
struct StaticsAabb {
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
};

/** How one mesh-extents array encoded its boxes. */
enum class StaticsExtentsKind : std::uint8_t {
    minMaxTriplets,
    minMaxVec4,
    centerExtentsTriplets,
    centerExtentsVec4,
    unresolved,
};

/** Outcome summary for one array parse, for logs and tests. */
struct StaticsParse {
    std::size_t stride{};
    std::size_t parsed{};
    /** Candidate records that passed validation; parsed + rejected = examined. */
    std::size_t rejected{};
    StaticsExtentsKind kind{StaticsExtentsKind::unresolved};
    bool valid{};
};

/**
 * Locates every valid array whose header names @p elementClass.
 * Descriptors are count/relative-offset pairs whose headers repeat the count and
 * carry tag-like class words, so only structurally sound arrays report.
 * @return The number written to @p output, capped at its size.
 */
[[nodiscard]] std::size_t find_statics_arrays(std::span<const std::byte> blob,
                                              std::uint32_t elementClass,
                                              std::span<StaticsArray> output) noexcept;

/**
 * Locates every valid array regardless of element class, for structure probes.
 * @return The number written to @p output, capped at its size.
 */
[[nodiscard]] std::size_t find_all_statics_arrays(std::span<const std::byte> blob,
                                                  std::span<StaticsArray> output) noexcept;

/**
 * Parses instance transforms with the stride inferred from validation.
 * A record is accepted when its quaternion is finite and near unit length, its
 * translation is finite with a destination-scale magnitude, and its scale is a
 * finite positive magnitude. The layout matches the engine's quaternion/position
 * record family (rotation at 0, translation at 16, scale at 28).
 */
[[nodiscard]] StaticsParse parse_statics_transforms(std::span<const std::byte> blob,
                                                    const StaticsArray& array,
                                                    std::span<StaticsTransform> output) noexcept;

/**
 * Parses mesh-extent boxes, inferring both stride and encoding.
 * Min/max encodings require ordered finite lanes; center/extents encodings
 * require positive finite extents.
 */
[[nodiscard]] StaticsParse parse_statics_extents(std::span<const std::byte> blob,
                                                 const StaticsArray& array,
                                                 std::span<StaticsAabb> output) noexcept;

/**
 * World-space box of one local box under one instance transform: every corner is
 * rotated by the quaternion, scaled, and translated.
 * @return False when the result is not finite; @p world is left untouched then.
 */
[[nodiscard]] bool transform_statics_aabb(const StaticsAabb& local,
                                          const StaticsTransform& placement,
                                          StaticsAabb& world) noexcept;

/**
 * Union of one local box placed by every given transform.
 * @return False when there are no placements or any placement is not finite.
 */
[[nodiscard]] bool union_statics_footprint(const StaticsAabb& local,
                                           std::span<const StaticsTransform> placements,
                                           StaticsAabb& world) noexcept;

/** One instance group: a mesh's run of instances (probe-confirmed 8-byte record). */
struct StaticsGroup {
    std::uint16_t instanceCount{};
    std::uint16_t instanceStart{};
    std::uint16_t meshIndex{};
    std::uint16_t reserved{};
};

/**
 * Reads one instance-group record out of a located groups array.
 */
[[nodiscard]] bool statics_group_at(std::span<const std::byte> blob,
                                    const StaticsArray& array,
                                    std::size_t index,
                                    StaticsGroup& output) noexcept;

/**
 * Reads one mesh tag out of a located mesh-tag array (bare 4-byte handles).
 */
[[nodiscard]] bool statics_mesh_tag_at(std::span<const std::byte> blob,
                                       const StaticsArray& array,
                                       std::size_t index,
                                       std::uint32_t& output) noexcept;

/**
 * Reads one mesh resource's model half-extents and expands them to a local box.
 * Rejects non-finite, non-positive, or implausibly large lanes instead of
 * guessing a substitute shape.
 */
[[nodiscard]] bool mesh_resource_extents(std::span<const std::byte> blob,
                                         StaticsAabb& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
