#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "config_scalar.h"

namespace sunrise::middleware::content::cui {
namespace {

using state::mods::ConfigBinding;
using state::mods::ConfigBindingProfile;
using state::mods::ConfigType;

/** Offsets and strides of the supported CUI resource layout. */
struct Layout final {
    /** Resource, typed array and comparison records use these wire offsets and widths. */
    static constexpr std::size_t kHeaderBytes = 344;
    static constexpr std::size_t kTypes = 56;
    static constexpr std::size_t kTypeStride = 16;
    static constexpr std::size_t kTypeNode = 10;
    static constexpr std::size_t kObjects = 72;
    static constexpr std::size_t kObjectStride = 64;
    static constexpr std::size_t kComponents = 32;
    static constexpr std::size_t kComponentStride = 24;
    static constexpr std::size_t kProperties = 8;
    static constexpr std::size_t kPropertyStride = 24;
    static constexpr std::size_t kSelector = 8;
    static constexpr std::size_t kValue = 16;
    static constexpr std::size_t kBindings = 88;
    static constexpr std::size_t kBindingStride = 56;
    static constexpr std::size_t kTransform = 48;
    static constexpr std::size_t kOperand = 4;
    static constexpr std::size_t kTransformBytes = 8;
    static constexpr std::size_t kArrayHeaderBytes = 16;
    static constexpr std::size_t kArrayClass = 8;
    static constexpr std::size_t kArrayPointer = 8;
    static constexpr std::size_t kMarkerBytes = 4;
};

/** Typed array and transform identifiers of the supported wire grammar. */
constexpr std::uint32_t kArrayMarker = 0x80809FBDU;
constexpr std::uint32_t kTypesClass = 0x80804622U;
constexpr std::uint32_t kObjectsClass = 0x8080462AU;
constexpr std::uint32_t kComponentsClass = 0x808046D4U;
constexpr std::uint32_t kPropertiesClass = 0x80804858U;
constexpr std::uint32_t kPathClass = 0x80804949U;
constexpr std::uint32_t kBindingsClass = 0x808046D8U;
constexpr std::uint32_t kNotClass = 0x808046E0U;
constexpr std::uint32_t kIntegerClass = 0x808046DCU;
constexpr std::uint32_t kFloatClass = 0x808046DBU;
constexpr std::uint32_t kHashClass = 0x808046DAU;
/** Typed arrays above 1M rows are refused. */
constexpr std::uint64_t kArrayLimit = 1U << 20;
/** Aliased nested arrays cannot consume unbounded startup work. */
constexpr std::size_t kVisitLimit = 4U << 20;
/** Native comparison codes occupy the inclusive range 0 through 5. */
constexpr std::uint8_t kLastComparison = 5;
/** Native comparison kinds are signed integer 1, float 2 and hash 3. */
constexpr std::uint8_t kIntegerKind = 1;
constexpr std::uint8_t kFloatKind = 2;
constexpr std::uint8_t kHashKind = 3;
/** Exact built-in object types supported by the scalar property profile. */
constexpr std::uint64_t kFrameType = 0xAE7E6D99ULL;
constexpr std::uint64_t kTextType = 0x0CE30D0BULL;
constexpr std::uint64_t kBitmapType = 0x7D1BD221ULL;
constexpr std::uint64_t kContainerType = 0x725AD1D887A7707BULL;
/** Direct base-layout coordinates and text auto-width selectors. */
constexpr std::uint64_t kFirstCoordinate = 0x206;
constexpr std::uint64_t kLastCoordinate = 0x209;
constexpr std::uint64_t kAutoWidth = 0x41A;

/** One array extent and its record width. */
struct Array final {
    std::size_t data{};
    std::size_t count{};
    std::size_t stride{};
};

struct PoolSchema final {
    std::size_t field;
    std::uint32_t classId;
    std::size_t stride;
};
/** Authored wire field definitions; opaque pool values are never interpreted as settings. */
constexpr std::array kPools{
    PoolSchema{136, 0x8080468DU, 1},
    PoolSchema{152, 0x80804696U, 4},
    PoolSchema{168, 0x80804695U, 8},
    PoolSchema{184, 0x80804694U, 4},
    PoolSchema{200, 0x80804694U, 4},
    PoolSchema{216, 0x80804693U, 16},
    PoolSchema{232, 0x80804692U, 4},
    PoolSchema{264, 0x80804690U, 4},
    PoolSchema{280, 0x8080468BU, 2},
    PoolSchema{312, 0x8080468FU, 4},
    PoolSchema{328, 0x80804688U, 16},
};
/** Pool descriptors accepted for boolean and float values. */
constexpr std::size_t kBytePool = 136;
constexpr std::size_t kFloatPoolA = 184;
constexpr std::size_t kFloatPoolB = 200;

/** Per-site facts accumulated while walking each resource array once. */
struct Match final {
    const ConfigBinding* site{};
    std::size_t references{};
    std::uint64_t node{};
    std::uint64_t selector{};
    std::uint64_t nodeType{};
    std::size_t typeMatches{};
};

/** Bounds-checked reader with one shared traversal budget. */
class Validator final {
public:
    Validator(std::span<const std::byte> bytes, std::span<const ConfigBinding> sites) noexcept
        : bytes_(bytes), sites_(sites) {}

    /** @return True when all sites have exactly one supported owner and reference. */
    bool run() noexcept;

private:
    /** @return True when the byte range is wholly inside the entry. */
    bool has(std::size_t at, std::size_t size) const noexcept {
        return at <= bytes_.size() && size <= bytes_.size() - at;
    }

    /** @param at Checked offset. @return The little-endian field. */
    template <typename T> T get(std::size_t at) const noexcept {
        T value{};
        std::memcpy(&value, bytes_.data() + at, sizeof value);
        return value;
    }

    /** @param site Typed site. @return Its fixed scalar width. */
    static std::size_t width(const ConfigBinding& site) noexcept {
        return site.type == ConfigType::boolean ? sizeof(std::uint8_t) : sizeof(std::uint32_t);
    }

    /**
     * Resolves a non-null relative pointer without signed overflow.
     * @param field Pointer word offset.
     * @param target Receives the absolute entry offset.
     * @return False on null or a target outside the entry.
     */
    bool relative(std::size_t field, std::size_t& target) const noexcept {
        if (!has(field, sizeof(std::int64_t))) {
            return false;
        }
        const auto delta = get<std::int64_t>(field);
        if (delta == 0 || delta < -static_cast<std::int64_t>(field)
            || delta >= static_cast<std::int64_t>(bytes_.size() - field)) {
            return false;
        }
        target = static_cast<std::size_t>(static_cast<std::int64_t>(field) + delta);
        return true;
    }

    /**
     * Finds the first configured scalar that intersects a range.
     * @param at Range offset.
     * @param size Range length.
     * @return Sorted-site index, or sites_.size() when none intersects.
     */
    std::size_t intersection(std::size_t at, std::size_t size) const noexcept {
        const auto ordered = std::span(order_).first(sites_.size());
        const auto found = std::lower_bound(
            ordered.begin(), ordered.end(), at, [](const Match* value, std::size_t start) {
                return value->site->offset + width(*value->site) <= start;
            });
        return found != ordered.end() && (*found)->site->offset < at + size
                   ? static_cast<std::size_t>(found - ordered.begin())
                   : sites_.size();
    }

    /** @return True when a structural range exists and contains no configured scalar. */
    bool structure(std::size_t at, std::size_t size) const noexcept {
        return has(at, size) && intersection(at, size) == sites_.size();
    }

    /**
     * Reads a typed array and protects its descriptor and prefix.
     * @param field Descriptor offset.
     * @param classId Expected element class.
     * @param stride Record size.
     * @param output Receives the bounded extent.
     * @param structural True when all record bytes are structural.
     * @return False on malformed metadata or scalar overlap.
     */
    bool array(std::size_t field,
               std::uint32_t classId,
               std::size_t stride,
               Array& output,
               bool structural = true) const noexcept {
        if (!structure(field, Layout::kArrayHeaderBytes)) {
            return false;
        }
        const auto count = get<std::uint64_t>(field);
        output = {0, 0, stride};
        if (count == 0) {
            return true;
        }
        std::size_t header = 0;
        if (count > kArrayLimit || !relative(field + Layout::kArrayPointer, header)
            || header < Layout::kMarkerBytes
            || !structure(header - Layout::kMarkerBytes,
                          Layout::kArrayHeaderBytes + Layout::kMarkerBytes)
            || get<std::uint32_t>(header - Layout::kMarkerBytes) != kArrayMarker
            || get<std::uint64_t>(header) != count
            || get<std::uint32_t>(header + Layout::kArrayClass) != classId) {
            return false;
        }
        output = {header + Layout::kArrayHeaderBytes, static_cast<std::size_t>(count), stride};
        const auto length = output.count * stride;
        return structural ? structure(output.data, length) : has(output.data, length);
    }

    /** @return False when repeated nested arrays exceed the shared work budget. */
    bool visit(std::size_t count) noexcept {
        if (count > remaining_) {
            return false;
        }
        remaining_ -= count;
        return true;
    }

    /** @return The unique typed pool containing an aligned value, or kPools.size(). */
    std::size_t pool_for(std::size_t value) const noexcept {
        std::size_t found = kPools.size();
        for (std::size_t index = 0; index < pools_.size(); ++index) {
            const auto& pool = pools_[index];
            if (value < pool.data || value - pool.data >= pool.count * pool.stride
                || (value - pool.data) % pool.stride != 0) {
                continue;
            }
            if (found != kPools.size()) {
                return kPools.size();
            }
            found = index;
        }
        return found;
    }

    bool properties() noexcept;
    bool property(const Array& rows, std::size_t index, std::uint64_t node) noexcept;
    bool comparisons() noexcept;
    bool types() noexcept;

    std::span<const std::byte> bytes_;
    std::span<const ConfigBinding> sites_;
    std::array<Match, state::mods::kConfigBindingLimit> matches_{};
    std::array<Match*, state::mods::kConfigBindingLimit> order_{};
    std::array<Array, kPools.size()> pools_{};
    std::size_t remaining_{kVisitLimit};
};

/**
 * Checks a property reference against any configured scalar it overlaps.
 * @param rows Owning reference array.
 * @param index Property row index.
 * @param node Owning component's local node key.
 * @return False when a site has another owner, path, pool or alias.
 */
bool Validator::property(const Array& rows, std::size_t index, std::uint64_t node) noexcept {
    const auto record = rows.data + index * rows.stride;
    std::size_t value = 0;
    if (!relative(record + Layout::kValue, value)) {
        return false;
    }
    const auto poolIndex = pool_for(value);
    if (poolIndex == kPools.size()) {
        return false;
    }
    const auto found = intersection(value, pools_[poolIndex].stride);
    if (found == sites_.size()) {
        return true;
    }
    Match& match = *order_[found];
    const ConfigBinding& site = *match.site;
    std::size_t pathField = 0;
    Array path{};
    if (site.profile != ConfigBindingProfile::cuiProperty || site.propertyOffset != record
        || site.offset != value || width(site) != pools_[poolIndex].stride
        || site.poolOffset != kPools[poolIndex].field || ++match.references != 1
        || !relative(record, pathField)
        || !array(pathField, kPathClass, sizeof(std::uint32_t), path) || path.count != 0
        || node > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    match.node = node;
    match.selector = get<std::uint64_t>(record + Layout::kSelector);
    for (std::size_t later = index + 1; later < rows.count; ++later) {
        const auto row = rows.data + later * rows.stride;
        if (get<std::uint64_t>(row + Layout::kSelector) != match.selector) {
            continue;
        }
        std::size_t laterPath = 0;
        Array selectors{};
        if (!relative(row, laterPath)
            || !array(laterPath, kPathClass, sizeof(std::uint32_t), selectors)
            || selectors.count == 0) {
            return false;
        }
    }
    if (site.type == ConfigType::boolean) {
        return site.poolOffset == kBytePool && match.selector == kAutoWidth
               && get<std::uint8_t>(value) <= 1;
    }
    return site.type == ConfigType::floating
           && (site.poolOffset == kFloatPoolA || site.poolOffset == kFloatPoolB)
           && match.selector >= kFirstCoordinate && match.selector <= kLastCoordinate
           && std::isfinite(get<float>(value));
}

/** @return True when every property has bounded arrays and isolated configured values. */
bool Validator::properties() noexcept {
    Array objects{};
    if (!array(Layout::kObjects, kObjectsClass, Layout::kObjectStride, objects)
        || !visit(objects.count)) {
        return false;
    }
    for (std::size_t object = 0; object < objects.count; ++object) {
        Array components{};
        if (!array(objects.data + object * objects.stride + Layout::kComponents,
                   kComponentsClass,
                   Layout::kComponentStride,
                   components)
            || !visit(components.count)) {
            return false;
        }
        for (std::size_t component = 0; component < components.count; ++component) {
            const auto at = components.data + component * components.stride;
            Array rows{};
            if (!array(at + Layout::kProperties, kPropertiesClass, Layout::kPropertyStride, rows)
                || !visit(rows.count)) {
                return false;
            }
            for (std::size_t index = 0; index < rows.count; ++index) {
                if (!property(rows, index, get<std::uint64_t>(at))) {
                    return false;
                }
            }
        }
    }
    return true;
}

/** @return True when comparison sites have their declared class, kind and unique binding row. */
bool Validator::comparisons() noexcept {
    Array bindings{};
    if (!array(Layout::kBindings, kBindingsClass, Layout::kBindingStride, bindings)
        || !visit(bindings.count)) {
        return false;
    }
    for (std::size_t index = 0; index < bindings.count; ++index) {
        const auto row = bindings.data + index * bindings.stride;
        if (get<std::int64_t>(row + Layout::kTransform) == 0) {
            continue;
        }
        std::size_t transform = 0;
        if (!relative(row + Layout::kTransform, transform) || transform < Layout::kMarkerBytes
            || !structure(transform - Layout::kMarkerBytes,
                          Layout::kMarkerBytes + Layout::kOperand)) {
            return false;
        }
        const auto classId = get<std::uint32_t>(transform - Layout::kMarkerBytes);
        const auto kind = get<std::uint8_t>(transform);
        if (classId == kNotClass && kind == 0) {
            continue;
        }
        if (!has(transform, Layout::kTransformBytes)
            || get<std::uint8_t>(transform + 1) > kLastComparison
            || !((classId == kIntegerClass && kind == kIntegerKind)
                 || (classId == kFloatClass && kind == kFloatKind)
                 || (classId == kHashClass && kind == kHashKind))) {
            return false;
        }
        const auto value = transform + Layout::kOperand;
        const auto found = intersection(value, sizeof(std::uint32_t));
        if (found == sites_.size()) {
            continue;
        }
        Match& match = *order_[found];
        const auto& site = *match.site;
        if (site.profile != ConfigBindingProfile::cuiComparison || site.bindingOffset != row
            || site.offset != value || width(site) != sizeof(std::uint32_t)
            || ++match.references != 1
            || !((site.type == ConfigType::integer && classId == kIntegerClass)
                 || (site.type == ConfigType::floating && classId == kFloatClass
                     && std::isfinite(get<float>(value))))) {
            return false;
        }
    }
    return true;
}

/** @return True when each property node joins one exact supported native type. */
bool Validator::types() noexcept {
    Array types{};
    if (!array(Layout::kTypes, kTypesClass, Layout::kTypeStride, types) || !visit(types.count)) {
        return false;
    }
    std::array<Match*, state::mods::kConfigBindingLimit> nodes{};
    std::size_t count = 0;
    for (std::size_t index = 0; index < sites_.size(); ++index) {
        if (matches_[index].site->profile == ConfigBindingProfile::cuiProperty) {
            nodes[count++] = &matches_[index];
        }
    }
    auto ordered = std::span(nodes).first(count);
    std::sort(ordered.begin(), ordered.end(), [](const Match* first, const Match* second) {
        return first->node < second->node;
    });
    for (std::size_t index = 0; index < types.count; ++index) {
        const auto at = types.data + index * types.stride;
        const auto node = get<std::uint16_t>(at + Layout::kTypeNode);
        auto found = std::lower_bound(
            ordered.begin(), ordered.end(), node, [](const Match* value, std::uint16_t key) {
                return value->node < key;
            });
        for (; found != ordered.end() && (*found)->node == node; ++found) {
            if (++(*found)->typeMatches != 1) {
                return false;
            }
            (*found)->nodeType = get<std::uint64_t>(at);
        }
    }
    return std::all_of(ordered.begin(), ordered.end(), [](const Match* value) {
        if (value->typeMatches != 1) {
            return false;
        }
        return value->site->type == ConfigType::boolean
                   ? value->nodeType == kTextType
                   : value->nodeType == kFrameType || value->nodeType == kTextType
                         || value->nodeType == kBitmapType || value->nodeType == kContainerType;
    });
}

/** @return True only after every configured byte has one verified scalar consumer. */
bool Validator::run() noexcept {
    if (sites_.empty() || sites_.size() > matches_.size() || bytes_.size() < Layout::kHeaderBytes
        || bytes_.size() > state::mods::kConfigEntryByteLimit) {
        return false;
    }
    for (std::size_t index = 0; index < sites_.size(); ++index) {
        const auto& site = sites_[index];
        if (site.classId != state::mods::kConfigCuiResourceClass || site.size != bytes_.size()
            || !has(site.offset, width(site))) {
            return false;
        }
        matches_[index].site = &site;
        order_[index] = &matches_[index];
    }
    auto ordered = std::span(order_).first(sites_.size());
    std::sort(ordered.begin(), ordered.end(), [](const Match* first, const Match* second) {
        return first->site->offset < second->site->offset;
    });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (ordered[index - 1]->site->offset + width(*ordered[index - 1]->site)
            > ordered[index]->site->offset) {
            return false;
        }
    }
    if (!structure(0, Layout::kHeaderBytes) || get<std::uint64_t>(0) != bytes_.size()) {
        return false;
    }
    for (std::size_t index = 0; index < kPools.size(); ++index) {
        const auto& schema = kPools[index];
        if (!array(schema.field, schema.classId, schema.stride, pools_[index], false)) {
            return false;
        }
    }
    return properties() && comparisons() && types()
           && std::all_of(matches_.begin(),
                          matches_.begin() + static_cast<std::ptrdiff_t>(sites_.size()),
                          [](const Match& value) { return value.references == 1; });
}

} // namespace

/** Validates one entry before any configured value is written. */
bool validate_config_scalars(std::span<const std::byte> bytes,
                             std::span<const state::mods::ConfigBinding> sites) noexcept {
    Validator validator(bytes, sites);
    return validator.run();
}

} // namespace sunrise::middleware::content::cui
