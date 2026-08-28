#include "activity_statevars.h"

#include <algorithm>
#include <set>

#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../middleware/content/packages/tables/internal.h"

namespace sunrise::client::content::activity::statevars {
namespace {

namespace catalog = inspection::activity_logic_catalog;
namespace tables = middleware::content::packages::tables;

constexpr std::uint32_t kCanonicalClass = 0x80809C04U;
constexpr std::uint32_t kDescriptorClass = 0x80809C20U;
constexpr std::uint32_t kOwnerClass = 0x80809C0FU;
constexpr std::uint32_t kDefinitionClass = 0x80809C36U;
constexpr std::uint32_t kComponentClass = 0x80804DE4U;
constexpr std::uint32_t kStateVarDescriptorClass = 0x80804DE8U;
constexpr std::uint32_t kTriggerClass = 0x80804DE7U;
constexpr std::uint32_t kProjectionBytecodeClass = 0x80800009U;
constexpr std::uint32_t kProjectionConstantClass = 0x80800090U;
constexpr std::size_t kMaximumRows = 250000;

[[nodiscard]] bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

[[nodiscard]] bool array_at(std::span<const std::byte> blob,
                            std::size_t descriptor,
                            std::uint32_t elementClass,
                            std::size_t stride,
                            bool optional,
                            tables::Array& output,
                            std::string& error) {
    output = {};
    std::uint64_t count = 0;
    std::int64_t relative = 0;
    if (!tables::read(blob, descriptor, count)
        || !tables::read(blob, descriptor + 8U, relative)) {
        return fail(error, "StateVar array descriptor is truncated");
    }
    if (count == 0) {
        if (optional && relative == 0) {
            return true;
        }
        return fail(error, "StateVar array descriptor is empty or malformed");
    }
    if (count > kMaximumRows || !tables::find_array_at(blob, descriptor, output)
        || output.elementClass != elementClass || output.dataOffset > blob.size()
        || count > (blob.size() - output.dataOffset) / stride) {
        return fail(error, "StateVar array descriptor is invalid");
    }
    return true;
}

} // namespace

bool parse_owner_rows(std::span<const std::byte> ownerBlob,
                      std::uint32_t ownerClass,
                      std::vector<OwnerRow>& rows,
                      std::vector<std::uint32_t>& canonicalConfigs,
                      std::string& error) {
    rows.clear();
    canonicalConfigs.clear();
    error.clear();
    if (ownerClass != kOwnerClass) {
        return fail(error, "StateVar owner class is invalid");
    }
    tables::Array canonical{};
    tables::Array descriptors{};
    if (!array_at(ownerBlob, 0x10, kCanonicalClass, 12, false, canonical, error)
        || !array_at(ownerBlob, 0x68, kDescriptorClass, 24, false, descriptors, error)) {
        return false;
    }
    std::set<std::uint32_t> canonicalConfigSet;
    for (std::uint64_t index = 0; index < canonical.count; ++index) {
        std::size_t offset = 0;
        std::uint32_t config = 0;
        if (!tables::element_offset(canonical.dataOffset, canonical.count, 12, index, offset)
            || !tables::read(ownerBlob, offset, config) || config == 0) {
            return fail(error, "StateVar canonical row is invalid");
        }
        if (!canonicalConfigSet.insert(config).second) {
            return fail(error, "StateVar canonical config is duplicated");
        }
        canonicalConfigs.push_back(config);
    }
    for (std::uint64_t index = 0; index < descriptors.count; ++index) {
        std::size_t offset = 0;
        std::uint32_t config = 0;
        std::uint32_t subtype = 0;
        std::uint32_t size = 0;
        if (!tables::element_offset(descriptors.dataOffset, descriptors.count, 24, index, offset)
            || !tables::read(ownerBlob, offset, config)
            || !tables::read(ownerBlob, offset + 4U, subtype)
            || !tables::read(ownerBlob, offset + 8U, size)) {
            return fail(error, "StateVar owner descriptor row is invalid");
        }
        if (subtype != kComponentClass) {
            continue;
        }
        if (size != 128U || !canonicalConfigSet.contains(config)) {
            return fail(error, "StateVar owner component row is not canonical");
        }
        if (std::ranges::find_if(rows, [config](const OwnerRow& row) {
                return row.configTag == config;
            }) == rows.end()) {
            rows.push_back({config});
        }
    }
    return true;
}

bool parse_config(std::span<const std::byte> configBlob,
                  std::uint32_t configClass,
                  std::uint32_t configTag,
                  catalog::StateVar& stateVar,
                  std::string& error) {
    error.clear();
    stateVar = {};
    if (configTag == 0 || configClass != kDefinitionClass) {
        return fail(error, "StateVar config identity is invalid");
    }
    std::size_t componentPayload = 0;
    std::size_t descriptorPayload = 0;
    std::uint32_t componentCount = 0;
    std::uint32_t descriptorCount = 0;
    for (const std::size_t field : {std::size_t{8}, std::size_t{16}, std::size_t{24}}) {
        std::int64_t relative = 0;
        if (!tables::read(configBlob, field, relative) || relative == 0) {
            continue;
        }
        const std::int64_t payload = static_cast<std::int64_t>(field) + relative;
        if (payload < 4 || static_cast<std::uint64_t>(payload) > configBlob.size()) {
            return fail(error, "StateVar config component offset is invalid");
        }
        std::uint32_t marker = 0;
        if (!tables::read(configBlob, static_cast<std::size_t>(payload) - 4U, marker)) {
            return fail(error, "StateVar config component marker is truncated");
        }
        if (marker == kComponentClass) {
            ++componentCount;
            componentPayload = static_cast<std::size_t>(payload);
        } else if (marker == kStateVarDescriptorClass) {
            ++descriptorCount;
            descriptorPayload = static_cast<std::size_t>(payload);
        }
    }
    if (componentCount != 1 || descriptorCount != 1) {
        return fail(error, "StateVar config components are not typed");
    }
    if (descriptorPayload <= componentPayload) {
        return fail(error, "StateVar descriptor precedes its component");
    }
    const std::size_t payload = descriptorPayload;
    tables::Array triggers{};
    tables::Array bytecode{};
    tables::Array constants{};
    if (!array_at(configBlob, payload + 0x78U, kTriggerClass, 24, true, triggers, error)
        || !array_at(configBlob,
                     payload + 0x98U,
                     kProjectionBytecodeClass,
                     1,
                     true,
                     bytecode,
                     error)
        || !array_at(configBlob,
                     payload + 0xA8U,
                     kProjectionConstantClass,
                     16,
                     true,
                     constants,
                     error)) {
        return false;
    }
    stateVar.configTag = configTag;
    if (!tables::read(configBlob, payload + 0x60U, stateVar.nameHash)
        || !tables::read(configBlob, payload + 0x64U, stateVar.initial)
        || !tables::read(configBlob, payload + 0x68U, stateVar.lowerClamp)
        || !tables::read(configBlob, payload + 0x6CU, stateVar.upperClamp)
        || !tables::read(configBlob, payload + 0x70U, stateVar.lowerThreshold)
        || !tables::read(configBlob, payload + 0x74U, stateVar.upperThreshold)) {
        return fail(error, "StateVar descriptor fields are truncated");
    }
    std::uint32_t projection = 0;
    if (!tables::read(configBlob, payload + 0x88U, projection)) {
        return fail(error, "StateVar projection flag is truncated");
    }
    if (projection > 1U || stateVar.nameHash == 0) {
        return fail(error, "StateVar scalar fields are invalid");
    }
    stateVar.projectionEnabled = projection != 0;
    stateVar.projectionBytecodeCount = static_cast<std::uint32_t>(bytecode.count);
    stateVar.projectionConstantCount = static_cast<std::uint32_t>(constants.count);
    stateVar.triggers.reserve(static_cast<std::size_t>(triggers.count));
    for (std::uint64_t index = 0; index < triggers.count; ++index) {
        std::size_t offset = 0;
        catalog::StateVarTrigger trigger{};
        if (!tables::element_offset(triggers.dataOffset, triggers.count, 24, index, offset)
            || !tables::read(configBlob, offset, trigger.lower)
            || !tables::read(configBlob, offset + 4U, trigger.upper)
            || !tables::read(configBlob, offset + 8U, trigger.referenceTag)
            || !tables::read(configBlob, offset + 16U, trigger.behaviorRootTag)
            || trigger.lower > trigger.upper || trigger.behaviorRootTag == 0) {
            return fail(error, "StateVar trigger row is invalid");
        }
        stateVar.triggers.push_back(trigger);
    }
    return true;
}

} // namespace sunrise::client::content::activity::statevars
