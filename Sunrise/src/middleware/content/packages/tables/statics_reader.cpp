#include "statics_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/** Descriptors name arrays no larger than the definition tables themselves. */
constexpr std::uint64_t kMaximumStaticsCount = 300000;
/** Descriptors and headers are 8-byte aligned structures. */
constexpr std::size_t kScanStep = 8;
/** A header sits at this offset relative to the descriptor's offset field. */
constexpr std::size_t kRelativeField = 8;
/** An array header repeats its count, names its class, then holds data. */
constexpr std::size_t kHeaderCountOffset = 0;
constexpr std::size_t kHeaderClassOffset = 8;
constexpr std::size_t kHeaderDataOffset = 16;
/** The class word before the header count carries this high half. */
constexpr std::size_t kHeaderMarkerBack = 4;
constexpr std::uint32_t kTagHighHalf = 0x8080;
/** Strides the transform record family uses across builds. */
constexpr std::array<std::size_t, 3> kTransformStrides{0x30, 0x38, 0x40};
/** Strides the extent box family uses across builds. */
constexpr std::array<std::size_t, 2> kExtentStrides{0x18, 0x20};
/** Records examined before the stride is accepted. */
constexpr std::size_t kProbeRecords = 16;
/** Fraction of examined records that must validate before a stride is accepted. */
constexpr float kAcceptance = 0.9F;
/** Translation magnitudes beyond this are not destination coordinates. */
constexpr float kMaximumTranslation = 1.0e6F;

[[nodiscard]] bool finite3(const std::array<float, 3>& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

[[nodiscard]] bool finite4(const std::array<float, 4>& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2])
           && std::isfinite(value[3]);
}

/** Rotates one vector by a quaternion using the standard sandwich product. */
void rotate(const std::array<float, 4>& q, std::array<float, 3>& v) noexcept {
    const float x = q[0];
    const float y = q[1];
    const float z = q[2];
    const float w = q[3];
    const float ix = w * v[0] + y * v[2] - z * v[1];
    const float iy = w * v[1] + z * v[0] - x * v[2];
    const float iz = w * v[2] + x * v[1] - y * v[0];
    const float iw = -x * v[0] - y * v[1] - z * v[2];
    v = {ix * w + iw * -x + iy * -z - iz * -y,
         iy * w + iw * -y + iz * -x - ix * -z,
         iz * w + iw * -z + ix * -y - iy * -x};
}

[[nodiscard]] bool quaternion_plausible(const std::array<float, 4>& q) noexcept {
    if (!finite4(q)) {
        return false;
    }
    const float squared = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return squared > 0.9F && squared < 1.1F;
}

[[nodiscard]] bool translation_plausible(const std::array<float, 3>& t) noexcept {
    if (!finite3(t)) {
        return false;
    }
    const float squared = t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
    return squared < kMaximumTranslation * kMaximumTranslation;
}

[[nodiscard]] bool scale_plausible(float s) noexcept {
    return std::isfinite(s) && s > 0.001F && s < 1000.0F;
}

/** Reads one transform record at a stride candidate. */
[[nodiscard]] bool read_transform(std::span<const std::byte> blob,
                                  std::size_t offset,
                                  StaticsTransform& output) noexcept {
    output = {};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        if (!read<float>(blob, offset + lane * sizeof(float), output.rotation[lane])) {
            return false;
        }
    }
    for (std::size_t lane = 0; lane < 3; ++lane) {
        if (!read<float>(blob, offset + 16U + lane * sizeof(float), output.translation[lane])) {
            return false;
        }
    }
    float scale = 1.0F;
    if (read<float>(blob, offset + 28U, scale) && scale_plausible(scale)) {
        output.scale = scale;
    }
    return quaternion_plausible(output.rotation) && translation_plausible(output.translation);
}

} // namespace

namespace {

/**
 * Walks every 8-byte slot as a candidate array descriptor.
 * @param requireClass When true, only arrays of @p elementClass report.
 */
std::size_t scan_statics_arrays(std::span<const std::byte> blob,
                                bool requireClass,
                                std::uint32_t elementClass,
                                std::span<StaticsArray> output) noexcept {
    std::size_t found = 0;
    if (blob.size() < kHeaderDataOffset + kHeaderMarkerBack) {
        return 0;
    }
    for (std::size_t offset = 0;
         offset + kRelativeField + sizeof(std::int64_t) <= blob.size() && found < output.size();
         offset += kScanStep) {
        std::uint64_t count = 0;
        std::int64_t relative = 0;
        if (!read<std::uint64_t>(blob, offset, count) || count == 0 || count > kMaximumStaticsCount
            || !read<std::int64_t>(blob, offset + kRelativeField, relative)) {
            continue;
        }
        const std::int64_t base = static_cast<std::int64_t>(offset + kRelativeField);
        if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
            || (relative < 0 && base < (std::numeric_limits<std::int64_t>::min)() - relative)) {
            continue;
        }
        const std::int64_t header = base + relative;
        if (header < static_cast<std::int64_t>(kHeaderMarkerBack)
            || static_cast<std::uint64_t>(header) + kHeaderDataOffset > blob.size()) {
            continue;
        }
        const auto headerOffset = static_cast<std::size_t>(header);
        std::uint64_t headerCount = 0;
        std::uint32_t marker = 0;
        std::uint32_t klass = 0;
        if (!read<std::uint64_t>(blob, headerOffset + kHeaderCountOffset, headerCount)
            || headerCount != count
            || !read<std::uint32_t>(blob, headerOffset - kHeaderMarkerBack, marker)
            || !read<std::uint32_t>(blob, headerOffset + kHeaderClassOffset, klass)
            || (marker >> 16U) != kTagHighHalf || (requireClass && klass != elementClass)) {
            continue;
        }
        output[found++] = StaticsArray{klass, count, headerOffset + kHeaderDataOffset, offset};
    }
    return found;
}

} // namespace

std::size_t find_statics_arrays(std::span<const std::byte> blob,
                                std::uint32_t elementClass,
                                std::span<StaticsArray> output) noexcept {
    return scan_statics_arrays(blob, true, elementClass, output);
}

std::size_t find_all_statics_arrays(std::span<const std::byte> blob,
                                    std::span<StaticsArray> output) noexcept {
    return scan_statics_arrays(blob, false, 0, output);
}

StaticsParse parse_statics_transforms(std::span<const std::byte> blob,
                                      const StaticsArray& array,
                                      std::span<StaticsTransform> output) noexcept {
    StaticsParse result{};
    if (array.count == 0) {
        return result;
    }
    const std::size_t examine = (std::min)(static_cast<std::size_t>(array.count), kProbeRecords);
    for (const std::size_t stride : kTransformStrides) {
        const std::uint64_t end =
            array.dataOffset + static_cast<std::uint64_t>(examine - 1U) * stride + 0x20U;
        if (end > blob.size()) {
            continue;
        }
        std::size_t accepted = 0;
        for (std::size_t index = 0; index < examine; ++index) {
            StaticsTransform probe{};
            if (read_transform(blob, array.dataOffset + index * stride, probe)) {
                ++accepted;
            }
        }
        if (static_cast<float>(accepted) >= kAcceptance * static_cast<float>(examine)) {
            const std::size_t capacity =
                (std::min)(output.size(), static_cast<std::size_t>(array.count));
            std::size_t parsed = 0;
            for (std::size_t index = 0; index < capacity; ++index) {
                if (read_transform(blob, array.dataOffset + index * stride, output[parsed])) {
                    ++parsed;
                }
            }
            result = StaticsParse{
                stride, parsed, examine - accepted, StaticsExtentsKind::unresolved, parsed != 0};
            return result;
        }
    }
    return result;
}

StaticsParse parse_statics_extents(std::span<const std::byte> blob,
                                   const StaticsArray& array,
                                   std::span<StaticsAabb> output) noexcept {
    struct Candidate final {
        StaticsExtentsKind kind;
        std::size_t stride;
        /** Offset of the second triple inside one record. */
        std::size_t secondOffset;
        bool minMax;
    };
    static constexpr std::array<Candidate, 4> kCandidates{{
        {StaticsExtentsKind::minMaxTriplets, 0x18, 12, true},
        {StaticsExtentsKind::minMaxVec4, 0x20, 16, true},
        {StaticsExtentsKind::centerExtentsTriplets, 0x18, 12, false},
        {StaticsExtentsKind::centerExtentsVec4, 0x20, 16, false},
    }};
    const auto readRecord = [&](const Candidate& candidate, std::size_t at, StaticsAabb& box) {
        std::array<float, 3> first{};
        std::array<float, 3> second{};
        bool valid = true;
        for (std::size_t lane = 0; lane < 3; ++lane) {
            valid = valid && read<float>(blob, at + lane * sizeof(float), first[lane]);
            valid = valid
                    && read<float>(
                        blob, at + candidate.secondOffset + lane * sizeof(float), second[lane]);
        }
        if (!valid || !finite3(first) || !finite3(second)) {
            return false;
        }
        for (std::size_t lane = 0; lane < 3; ++lane) {
            if (candidate.minMax) {
                if (first[lane] > second[lane]) {
                    return false;
                }
            } else if (second[lane] < 0.0F) {
                return false;
            }
        }
        box = candidate.minMax
                  ? StaticsAabb{first, second}
                  : StaticsAabb{{first[0] - second[0], first[1] - second[1], first[2] - second[2]},
                                {first[0] + second[0], first[1] + second[1], first[2] + second[2]}};
        return true;
    };

    StaticsParse result{};
    if (array.count == 0) {
        return result;
    }
    const std::size_t examine = (std::min)(static_cast<std::size_t>(array.count), kProbeRecords);
    for (const Candidate& candidate : kCandidates) {
        const std::uint64_t end = array.dataOffset
                                  + static_cast<std::uint64_t>(examine - 1U) * candidate.stride
                                  + candidate.secondOffset + 12U;
        if (end > blob.size()) {
            continue;
        }
        std::size_t accepted = 0;
        for (std::size_t index = 0; index < examine; ++index) {
            StaticsAabb probe{};
            if (readRecord(candidate, array.dataOffset + index * candidate.stride, probe)) {
                ++accepted;
            }
        }
        if (static_cast<float>(accepted) < kAcceptance * static_cast<float>(examine)) {
            continue;
        }
        const std::size_t capacity =
            (std::min)(output.size(), static_cast<std::size_t>(array.count));
        std::size_t parsed = 0;
        for (std::size_t index = 0; index < capacity && parsed < output.size(); ++index) {
            if (readRecord(
                    candidate, array.dataOffset + index * candidate.stride, output[parsed])) {
                ++parsed;
            }
        }
        return StaticsParse{
            candidate.stride, parsed, examine - accepted, candidate.kind, parsed != 0};
    }
    return result;
}

bool statics_group_at(std::span<const std::byte> blob,
                      const StaticsArray& array,
                      std::size_t index,
                      StaticsGroup& output) noexcept {
    output = {};
    if (index >= array.count) {
        return false;
    }
    const std::size_t at = array.dataOffset + index * 8U;
    return read<std::uint16_t>(blob, at, output.instanceCount)
           && read<std::uint16_t>(blob, at + 2U, output.instanceStart)
           && read<std::uint16_t>(blob, at + 4U, output.meshIndex)
           && read<std::uint16_t>(blob, at + 6U, output.reserved);
}

bool statics_mesh_tag_at(std::span<const std::byte> blob,
                         const StaticsArray& array,
                         std::size_t index,
                         std::uint32_t& output) noexcept {
    output = 0;
    if (index >= array.count) {
        return false;
    }
    return read<std::uint32_t>(blob, array.dataOffset + index * 4U, output);
}

bool mesh_resource_extents(std::span<const std::byte> blob, StaticsAabb& output) noexcept {
    std::array<float, 3> extents{};
    for (std::size_t lane = 0; lane < 3; ++lane) {
        if (!read<float>(blob, kMeshExtentsOffset + lane * sizeof(float), extents[lane])) {
            return false;
        }
    }
    if (!finite3(extents)) {
        return false;
    }
    for (const float lane : extents) {
        if (lane <= 0.0F || lane >= kMeshExtentLimit) {
            return false;
        }
    }
    output =
        StaticsAabb{{-extents[0], -extents[1], -extents[2]}, {extents[0], extents[1], extents[2]}};
    return true;
}

bool transform_statics_aabb(const StaticsAabb& local,
                            const StaticsTransform& placement,
                            StaticsAabb& world) noexcept {
    StaticsAabb candidate{};
    candidate.minimum.fill(std::numeric_limits<float>::max());
    candidate.maximum.fill(-std::numeric_limits<float>::max());
    for (std::size_t corner = 0; corner < 8; ++corner) {
        std::array<float, 3> point{corner % 2U == 0U ? local.minimum[0] : local.maximum[0],
                                   (corner / 2U) % 2U == 0U ? local.minimum[1] : local.maximum[1],
                                   corner / 4U == 0U ? local.minimum[2] : local.maximum[2]};
        rotate(placement.rotation, point);
        point = {point[0] * placement.scale + placement.translation[0],
                 point[1] * placement.scale + placement.translation[1],
                 point[2] * placement.scale + placement.translation[2]};
        if (!finite3(point)) {
            return false;
        }
        for (std::size_t lane = 0; lane < 3; ++lane) {
            candidate.minimum[lane] = (std::min)(candidate.minimum[lane], point[lane]);
            candidate.maximum[lane] = (std::max)(candidate.maximum[lane], point[lane]);
        }
    }
    world = candidate;
    return true;
}

bool union_statics_footprint(const StaticsAabb& local,
                             std::span<const StaticsTransform> placements,
                             StaticsAabb& world) noexcept {
    if (placements.empty()) {
        return false;
    }
    StaticsAabb combined{};
    combined.minimum.fill(std::numeric_limits<float>::max());
    combined.maximum.fill(-std::numeric_limits<float>::max());
    for (const StaticsTransform& placement : placements) {
        StaticsAabb placed{};
        if (!transform_statics_aabb(local, placement, placed)) {
            return false;
        }
        for (std::size_t lane = 0; lane < 3; ++lane) {
            combined.minimum[lane] = (std::min)(combined.minimum[lane], placed.minimum[lane]);
            combined.maximum[lane] = (std::max)(combined.maximum[lane], placed.maximum[lane]);
        }
    }
    world = combined;
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
