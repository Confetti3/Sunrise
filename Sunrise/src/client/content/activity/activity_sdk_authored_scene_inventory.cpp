#include "activity_sdk_authored_scene_inventory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {
namespace {

namespace tables = middleware::content::packages::tables;

/** Hash combining uses the 32-bit golden-ratio increment. */
constexpr std::size_t kHashCombineConstant = 0x9E3779B9U;
/** The target slot type follows the object key in a scene reference. */
constexpr std::size_t kTargetSlotTypeRelativeOffset = 4U;
/** The target slot index follows the target slot type. */
constexpr std::size_t kTargetSlotIndexRelativeOffset = 6U;

/** One cached package entry and its physical class. */
struct PackageRow final {
    std::vector<std::byte> bytes{};
    std::uint32_t classId{};
};

/** One exact slot schema indexed by the canonical global slot row. */
using SchemaIndex = std::unordered_map<std::uint32_t, const squad::SlotSchemaFact*>;
/** Package reads are cached by tag for one transaction. */
using PackageCache = std::unordered_map<std::uint32_t, PackageRow>;

/** @return True when one complete range lies in the package blob. */
[[nodiscard]] bool
contains(std::span<const std::byte> blob, std::size_t offset, std::size_t size) noexcept {
    return offset <= blob.size() && size <= blob.size() - offset;
}

template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, Value& output) noexcept {
    output = {};
    if (!contains(blob, offset, sizeof output)) {
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

/** Formats one structural ID without truncation. */
template <typename... Values>
[[nodiscard]] bool format_text(Text& output, const char* spec, Values... values) noexcept {
    output = {};
    const int written = std::snprintf(output.value.data(), output.value.size(), spec, values...);
    if (written <= 0 || static_cast<std::size_t>(written) >= output.value.size()) {
        output = {};
        return false;
    }
    output.length = static_cast<std::uint16_t>(written);
    return true;
}

/** @return True when all fixed string storage follows the deferred-text contract. */
[[nodiscard]] bool valid_text(const Text& text) noexcept {
    if (text.length >= text.value.size() || text.value[text.length] != '\0') {
        return false;
    }
    for (std::size_t index = 0; index < text.length; ++index) {
        if (text.value[index] == '\0') {
            return false;
        }
    }
    return std::all_of(text.value.begin() + text.length + 1U, text.value.end(), [](char value) {
        return value == '\0';
    });
}

/** Returns one deferred string after validating its fixed storage. */
[[nodiscard]] bool text_view(const Text& text, std::string_view& output) noexcept {
    output = {};
    if (!valid_text(text)) {
        return false;
    }
    output = std::string_view(text.value.data(), text.length);
    return true;
}

/** Derives the descriptor identity used by every authored-scene child row. */
[[nodiscard]] bool descriptor_id(const topology::Snapshot& topology,
                                 const squad::DescriptorFact& descriptor,
                                 std::string& output) {
    output.clear();
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    if (slot.objectIndex != descriptor.objectIndex) {
        return false;
    }
    std::array<char, 80> buffer{};
    const int written = std::snprintf(buffer.data(),
                                      buffer.size(),
                                      "descriptor/%08x/%08x/%08x/%04x/%04x",
                                      static_cast<unsigned>(descriptor.configTag),
                                      static_cast<unsigned>(object.objectTag),
                                      static_cast<unsigned>(descriptor.descriptorOffset),
                                      static_cast<unsigned>(slot.slotIndex),
                                      static_cast<unsigned>(slot.slotType));
    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
        return false;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(written));
    return true;
}

/** Formats one resource ID from its exact descriptor tuple. */
[[nodiscard]] bool resource_id(const topology::Snapshot& topology,
                               const squad::DescriptorFact& descriptor,
                               Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    return format_text(output,
                       "authored-scene-resource/%08x/%08x/%08x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType));
}

/** Formats one scene-to-squad edge ID from its exact descriptor tuple. */
[[nodiscard]] bool edge_id(const topology::Snapshot& topology,
                           const squad::DescriptorFact& descriptor,
                           Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    return format_text(output,
                       "authored-scene-squad-edge/%08x/%08x/%08x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType));
}

/** Formats one task-to-objective target ID from its exact descriptor tuple. */
[[nodiscard]] bool task_target_id(const topology::Snapshot& topology,
                                  const squad::DescriptorFact& descriptor,
                                  Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    return format_text(output,
                       "task-target/%08x/%08x/%08x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType));
}

/** Tests the exact type-43 descriptor shape before package projection. */
[[nodiscard]] bool is_scene_descriptor(const topology::Snapshot& topology,
                                       const squad::DescriptorFact& descriptor) noexcept {
    return descriptor.slotIndex < topology.slots.size()
           && topology.slots[descriptor.slotIndex].slotType == format::kAuthoredSceneSlotType
           && descriptor.componentClass == format::kAuthoredSceneComponentClass
           && descriptor.senseSchema == format::kAuthoredSceneSenseSchema
           && descriptor.authSchema == format::kAuthoredSceneAuthSchema;
}

/** Names the guard that refused one type-42 sensor, so one run can say why an idle has no state. */
void log_performance_edge(const squad::DescriptorFact& descriptor, const char* result) noexcept {
    std::array<char, 176> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity_sdk_performance_edge result=%s config=0x%08X offset=0x%X "
                      "slot_row=%u",
                      result,
                      static_cast<unsigned>(descriptor.configTag),
                      static_cast<unsigned>(descriptor.descriptorOffset),
                      static_cast<unsigned>(descriptor.slotIndex));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::debug,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
}

/** Tests the exact type-42 descriptor shape before package projection. */
[[nodiscard]] bool is_performance_descriptor(const topology::Snapshot& topology,
                                             const squad::DescriptorFact& descriptor) noexcept {
    return descriptor.slotIndex < topology.slots.size()
           && topology.slots[descriptor.slotIndex].slotType == format::kPerformanceSlotType
           && descriptor.componentClass == format::kPerformanceComponentClass
           && descriptor.senseSchema == format::kAbsentIndex
           && descriptor.authSchema == format::kPerformanceAuthSchema;
}

/** Tests the exact type-38 descriptor shape before package projection. */
[[nodiscard]] bool is_task_descriptor(const topology::Snapshot& topology,
                                      const squad::DescriptorFact& descriptor) noexcept {
    return descriptor.slotIndex < topology.slots.size()
           && topology.slots[descriptor.slotIndex].slotType == format::kTaskSlotType
           && descriptor.componentClass == format::kTaskComponentClass
           && descriptor.senseSchema == format::kAbsentIndex
           && descriptor.authSchema == format::kTaskAuthSchema;
}

/** Tests one exact final slot shape supplied by the separate schema join. */
[[nodiscard]] bool slot_shape(const topology::Snapshot& topology,
                              const SchemaIndex& schemas,
                              std::uint32_t slotIndex,
                              std::uint32_t slotType,
                              std::uint32_t componentClass,
                              std::uint32_t senseSchema,
                              std::uint32_t authSchema) noexcept {
    if (slotIndex >= topology.slots.size()) {
        return false;
    }
    const auto found = schemas.find(slotIndex);
    if (found == schemas.end() || found->second == nullptr || !found->second->exact) {
        return false;
    }
    const topology::Slot& slot = topology.slots[slotIndex];
    const squad::SlotSchemaFact& schema = *found->second;
    return slot.slotType == slotType && schema.slotIndex == slotIndex
           && schema.componentClass == componentClass && schema.senseSchema == senseSchema
           && schema.authSchema == authSchema;
}

/** Reads one tag once and retains the physical class beside its bytes. */
[[nodiscard]] bool package_row(squad::TagReader reader,
                               void* readerContext,
                               std::uint32_t tag,
                               PackageCache& cache,
                               const PackageRow*& output) {
    output = nullptr;
    const auto found = cache.find(tag);
    if (found != cache.end()) {
        output = &found->second;
        return true;
    }
    PackageRow row{};
    if (tag == 0 || tag == format::kAbsentIndex
        || !reader(readerContext, tag, row.bytes, row.classId)) {
        return false;
    }
    const auto [inserted, accepted] = cache.emplace(tag, std::move(row));
    if (!accepted) {
        return false;
    }
    output = &inserted->second;
    return true;
}

/** Finds one globally unique target slot by the authored client-reference triple. */
[[nodiscard]] bool target_slot(const topology::Snapshot& topology,
                               const squad::DescriptorFact& descriptor,
                               std::uint32_t targetObjectKey,
                               std::uint16_t targetSlotType,
                               std::uint16_t targetSlotIndex,
                               std::uint32_t& output,
                               bool& found) noexcept {
    output = format::kAbsentIndex;
    found = false;
    if (descriptor.objectIndex >= topology.objects.size()) {
        return false;
    }
    for (std::uint32_t objectIndex = 0; objectIndex < topology.objects.size(); ++objectIndex) {
        const topology::Object& object = topology.objects[objectIndex];
        if (object.objectKey != targetObjectKey) {
            continue;
        }
        if (object.firstSlot > topology.slots.size()
            || object.slotCount > topology.slots.size() - object.firstSlot) {
            return false;
        }
        for (std::uint32_t index = object.firstSlot; index < object.firstSlot + object.slotCount;
             ++index) {
            const topology::Slot& slot = topology.slots[index];
            if (slot.objectIndex != objectIndex || slot.slotType != targetSlotType
                || slot.slotIndex != targetSlotIndex) {
                continue;
            }
            if (found) {
                output = format::kAbsentIndex;
                found = false;
                return true;
            }
            output = index;
            found = true;
        }
    }
    return true;
}

/** Finds one target slot inside the descriptor's owning object. */
[[nodiscard]] bool same_object_slot(const topology::Snapshot& topology,
                                    const squad::DescriptorFact& descriptor,
                                    std::uint16_t targetSlotType,
                                    std::uint16_t targetSlotIndex,
                                    std::uint32_t& output) noexcept {
    output = format::kAbsentIndex;
    if (descriptor.objectIndex >= topology.objects.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    if (object.firstSlot > topology.slots.size()
        || object.slotCount > topology.slots.size() - object.firstSlot) {
        return false;
    }
    for (std::uint32_t index = object.firstSlot; index < object.firstSlot + object.slotCount;
         ++index) {
        const topology::Slot& slot = topology.slots[index];
        if (slot.objectIndex == descriptor.objectIndex && slot.slotType == targetSlotType
            && slot.slotIndex == targetSlotIndex) {
            output = index;
            return true;
        }
    }
    return true;
}

/** Builds and validates the unique global schema lookup. */
[[nodiscard]] bool
schema_index(const topology::Snapshot& topology, const Facts& facts, SchemaIndex& output) {
    output.clear();
    try {
        output.reserve(facts.slotSchemas.size());
        for (const squad::SlotSchemaFact& schema : facts.slotSchemas) {
            if (schema.slotIndex >= topology.slots.size()
                || !output.emplace(schema.slotIndex, &schema).second) {
                return false;
            }
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Checks the topology fields consumed by this bounded projection. */
[[nodiscard]] bool valid_topology(const topology::Snapshot& topology) noexcept {
    if (!topology.ready || topology.objects.empty() || topology.slots.empty()) {
        return false;
    }
    std::size_t nextSlot = 0;
    for (std::size_t objectIndex = 0; objectIndex < topology.objects.size(); ++objectIndex) {
        const topology::Object& object = topology.objects[objectIndex];
        if (object.objectTag == 0 || object.objectTag == format::kAbsentIndex
            || object.objectKey == 0 || object.objectKey == format::kAbsentIndex
            || object.firstSlot != nextSlot || object.firstSlot > topology.slots.size()
            || object.slotCount > topology.slots.size() - object.firstSlot) {
            return false;
        }
        for (std::uint32_t index = object.firstSlot; index < object.firstSlot + object.slotCount;
             ++index) {
            const topology::Slot& slot = topology.slots[index];
            if (slot.objectIndex != objectIndex || slot.slotIndex != index - object.firstSlot) {
                return false;
            }
        }
        nextSlot += object.slotCount;
    }
    return nextSlot == topology.slots.size();
}

/** Checks the complete descriptor set before package reads begin. */
[[nodiscard]] bool valid_facts(const topology::Snapshot& topology, const Facts& facts) {
    if (!facts.complete || facts.slotSchemas.size() != topology.slots.size()) {
        return false;
    }
    std::unordered_set<std::string> ids{};
    std::unordered_set<std::uint32_t> schemaSlots{};
    try {
        ids.reserve(facts.descriptors.size());
        schemaSlots.reserve(facts.slotSchemas.size());
        std::vector<std::size_t> descriptorCounts(topology.objects.size());
        std::vector<bool> projectedOwners(topology.objects.size());
        for (const squad::SlotSchemaFact& schema : facts.slotSchemas) {
            if (schema.slotIndex >= topology.slots.size()
                || !schemaSlots.emplace(schema.slotIndex).second) {
                return false;
            }
            const topology::Slot& slot = topology.slots[schema.slotIndex];
            if (slot.objectIndex >= topology.objects.size()) {
                return false;
            }
            if (slot.slotType == format::kAuthoredSceneSlotType && schema.exact
                && schema.componentClass == format::kAuthoredSceneComponentClass
                && schema.senseSchema == format::kAuthoredSceneSenseSchema
                && schema.authSchema == format::kAuthoredSceneAuthSchema) {
                projectedOwners[slot.objectIndex] = true;
            }
            if (slot.slotType == format::kTaskSlotType && schema.exact
                && schema.componentClass == format::kTaskComponentClass
                && schema.senseSchema == format::kAbsentIndex
                && schema.authSchema == format::kTaskAuthSchema) {
                projectedOwners[slot.objectIndex] = true;
            }
        }
        for (const squad::DescriptorFact& descriptor : facts.descriptors) {
            std::string expected{};
            if (!descriptor.complete || descriptor.configTag == 0
                || descriptor.configTag == format::kAbsentIndex
                || descriptor.objectIndex >= topology.objects.size()
                || descriptor.slotIndex >= topology.slots.size()
                || descriptor.descriptorOffset
                       > format::kAbsentIndex - format::kAuthoredSceneSquadReferenceRelativeOffset
                || !descriptor_id(topology, descriptor, expected) || descriptor.id != expected
                || !ids.emplace(descriptor.id).second) {
                return false;
            }
            ++descriptorCounts[descriptor.objectIndex];
        }
        for (std::size_t index = 0; index < topology.objects.size(); ++index) {
            const topology::Object& object = topology.objects[index];
            if (projectedOwners[index]
                && (!object.descriptorEvidenceComplete
                    || object.descriptorCount == format::kAbsentIndex
                    || descriptorCounts[index] != object.descriptorCount)) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/** Sort key matching the final section-19 pack order. */
[[nodiscard]] auto resource_natural(const Resource& row) noexcept {
    return std::tie(row.slotIndex, row.configTag, row.descriptorOffset);
}

/** Sort key matching the final section-22 pack order. */
[[nodiscard]] auto edge_natural(const SquadEdge& row) noexcept {
    return std::tie(row.sceneSlotIndex, row.configTag, row.descriptorOffset);
}

/** Sort key matching the final task-target pack order. */
[[nodiscard]] auto task_natural(const TaskTarget& row) noexcept {
    return std::tie(row.taskSlotIndex, row.configTag, row.descriptorOffset);
}

/** Compares resource rows in final pack order. */
[[nodiscard]] bool resource_less(const Resource& left, const Resource& right) noexcept {
    std::string_view leftId{};
    std::string_view rightId{};
    if (!text_view(left.id, leftId) || !text_view(right.id, rightId)) {
        return false;
    }
    return std::tie(left.slotIndex, left.configTag, left.descriptorOffset, left.resourceTag, leftId)
           < std::tie(right.slotIndex,
                      right.configTag,
                      right.descriptorOffset,
                      right.resourceTag,
                      rightId);
}

/** Compares edge rows in final pack order. */
[[nodiscard]] bool edge_less(const SquadEdge& left, const SquadEdge& right) noexcept {
    std::string_view leftId{};
    std::string_view rightId{};
    if (!text_view(left.id, leftId) || !text_view(right.id, rightId)) {
        return false;
    }
    return std::tie(left.sceneSlotIndex,
                    left.configTag,
                    left.descriptorOffset,
                    left.squadSlotIndex,
                    leftId)
           < std::tie(right.sceneSlotIndex,
                      right.configTag,
                      right.descriptorOffset,
                      right.squadSlotIndex,
                      rightId);
}

/** Compares task-target rows in final pack order. */
[[nodiscard]] bool task_less(const TaskTarget& left, const TaskTarget& right) noexcept {
    std::string_view leftId{};
    std::string_view rightId{};
    if (!text_view(left.id, leftId) || !text_view(right.id, rightId)) {
        return false;
    }
    return std::tie(left.taskSlotIndex,
                    left.configTag,
                    left.descriptorOffset,
                    left.objectiveSlotIndex,
                    left.bitIndex,
                    leftId)
           < std::tie(right.taskSlotIndex,
                      right.configTag,
                      right.descriptorOffset,
                      right.objectiveSlotIndex,
                      right.bitIndex,
                      rightId);
}

/** One descriptor lookup key matches the final natural identity. */
struct DescriptorKey final {
    std::uint32_t slotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};

    bool operator==(const DescriptorKey&) const = default;
};

/** Mixes the three exact u32 identity lanes without narrowing them. */
struct DescriptorKeyHash final {
    [[nodiscard]] std::size_t operator()(const DescriptorKey& value) const noexcept {
        std::size_t result = value.slotIndex;
        result ^= static_cast<std::size_t>(value.configTag) + kHashCombineConstant + (result << 6U)
                  + (result >> 2U);
        result ^= static_cast<std::size_t>(value.descriptorOffset) + kHashCombineConstant
                  + (result << 6U) + (result >> 2U);
        return result;
    }
};

using DescriptorIndex =
    std::unordered_map<DescriptorKey, const squad::DescriptorFact*, DescriptorKeyHash>;

/** Builds the exact descriptor lookup consumed by output validation. */
[[nodiscard]] bool descriptor_index(const Facts& facts, DescriptorIndex& output) {
    output.clear();
    try {
        output.reserve(facts.descriptors.size());
        for (const squad::DescriptorFact& descriptor : facts.descriptors) {
            const DescriptorKey key{
                descriptor.slotIndex, descriptor.configTag, descriptor.descriptorOffset};
            if (!output.emplace(key, &descriptor).second) {
                return false;
            }
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Finds one descriptor fact by its natural package identity. */
[[nodiscard]] const squad::DescriptorFact*
find_descriptor(const DescriptorIndex& descriptors,
                std::uint32_t slotIndex,
                std::uint32_t configTag,
                std::uint32_t descriptorOffset) noexcept {
    const auto found = descriptors.find({slotIndex, configTag, descriptorOffset});
    return found == descriptors.end() ? nullptr : found->second;
}

} // namespace

/** Validates every structural ID, scalar domain, slot join, flag, and row order. */
bool validate(const topology::Snapshot& topology,
              const Facts& facts,
              const Snapshot& snapshot) noexcept {
    if (!snapshot.complete || !valid_topology(topology) || !valid_facts(topology, facts)) {
        return false;
    }
    SchemaIndex schemas{};
    DescriptorIndex descriptors{};
    if (!schema_index(topology, facts, schemas) || !descriptor_index(facts, descriptors)) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.resources.size(); ++index) {
        const Resource& row = snapshot.resources[index];
        const squad::DescriptorFact* descriptor =
            find_descriptor(descriptors, row.slotIndex, row.configTag, row.descriptorOffset);
        Text expectedId{};
        if (descriptor == nullptr || !is_scene_descriptor(topology, *descriptor)
            || !resource_id(topology, *descriptor, expectedId) || expectedId.value != row.id.value
            || expectedId.length != row.id.length
            || !slot_shape(topology,
                           schemas,
                           row.slotIndex,
                           format::kAuthoredSceneSlotType,
                           format::kAuthoredSceneComponentClass,
                           format::kAuthoredSceneSenseSchema,
                           format::kAuthoredSceneAuthSchema)
            || row.descriptorOffset
                   > format::kAbsentIndex - format::kAuthoredSceneResourceRelativeOffset
            || row.resourceFieldOffset
                   != row.descriptorOffset + format::kAuthoredSceneResourceRelativeOffset
            || row.resourceTag == 0 || row.resourceTag == format::kAbsentIndex
            || row.resourceClass != format::kAuthoredSceneResourceClass
            || row.flags != format::kAuthoredSceneResourceExact || row.reserved != 0
            || (index != 0 && !resource_less(snapshot.resources[index - 1], row))) {
            return false;
        }
    }
    for (std::size_t index = 1; index < snapshot.resources.size(); ++index) {
        if (resource_natural(snapshot.resources[index - 1])
            == resource_natural(snapshot.resources[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.squadEdges.size(); ++index) {
        const SquadEdge& row = snapshot.squadEdges[index];
        const squad::DescriptorFact* descriptor =
            find_descriptor(descriptors, row.sceneSlotIndex, row.configTag, row.descriptorOffset);
        Text expectedId{};
        // A row is a type-43 scene edge or a type-42 performance edge; the flag says which.
        const bool performance = row.flags == format::kAuthoredSceneSquadPerformanceTargetExact;
        const std::uint32_t referenceOffset =
            performance ? format::kPerformanceSquadReferenceRelativeOffset
                        : format::kAuthoredSceneSquadReferenceRelativeOffset;
        const bool sourceShape = performance ? slot_shape(topology,
                                                          schemas,
                                                          row.sceneSlotIndex,
                                                          format::kPerformanceSlotType,
                                                          format::kPerformanceComponentClass,
                                                          format::kAbsentIndex,
                                                          format::kPerformanceAuthSchema)
                                             : slot_shape(topology,
                                                          schemas,
                                                          row.sceneSlotIndex,
                                                          format::kAuthoredSceneSlotType,
                                                          format::kAuthoredSceneComponentClass,
                                                          format::kAuthoredSceneSenseSchema,
                                                          format::kAuthoredSceneAuthSchema);
        if (descriptor == nullptr || descriptor->objectIndex >= topology.objects.size()
            || row.squadSlotIndex >= topology.slots.size()
            || topology.slots[row.squadSlotIndex].objectIndex != descriptor->objectIndex
            || !(performance ? is_performance_descriptor(topology, *descriptor)
                             : is_scene_descriptor(topology, *descriptor))
            || !edge_id(topology, *descriptor, expectedId) || expectedId.value != row.id.value
            || expectedId.length != row.id.length || !sourceShape
            || !slot_shape(topology,
                           schemas,
                           row.squadSlotIndex,
                           format::kSquadSlotType,
                           format::kSquadComponentClass,
                           format::kSquadSenseSchema,
                           format::kSquadAuthSchema)
            || row.descriptorOffset > format::kAbsentIndex - referenceOffset
            || row.referenceFieldOffset != row.descriptorOffset + referenceOffset
            || row.targetObjectKey != topology.objects[descriptor->objectIndex].objectKey
            || (row.flags != format::kAuthoredSceneSquadSameObjectExact && !performance)
            || row.reserved != 0
            || (index != 0 && !edge_less(snapshot.squadEdges[index - 1], row))) {
            return false;
        }
    }
    for (std::size_t index = 1; index < snapshot.squadEdges.size(); ++index) {
        if (edge_natural(snapshot.squadEdges[index - 1])
            == edge_natural(snapshot.squadEdges[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.taskTargets.size(); ++index) {
        const TaskTarget& row = snapshot.taskTargets[index];
        const squad::DescriptorFact* descriptor =
            find_descriptor(descriptors, row.taskSlotIndex, row.configTag, row.descriptorOffset);
        Text expectedId{};
        if (descriptor == nullptr || descriptor->objectIndex >= topology.objects.size()
            || row.objectiveSlotIndex >= topology.slots.size()
            || !is_task_descriptor(topology, *descriptor)
            || !task_target_id(topology, *descriptor, expectedId)
            || expectedId.value != row.id.value || expectedId.length != row.id.length
            || !slot_shape(topology,
                           schemas,
                           row.taskSlotIndex,
                           format::kTaskSlotType,
                           format::kTaskComponentClass,
                           format::kAbsentIndex,
                           format::kTaskAuthSchema)
            || !slot_shape(topology,
                           schemas,
                           row.objectiveSlotIndex,
                           format::kObjectiveSlotType,
                           format::kObjectiveComponentClass,
                           format::kObjectiveSenseSchema,
                           format::kObjectiveAuthSchema)
            || row.descriptorOffset > format::kAbsentIndex - format::kTaskBitIndexRelativeOffset
            || row.referenceFieldOffset
                   != row.descriptorOffset + format::kTaskReferenceRelativeOffset
            || topology.slots[row.objectiveSlotIndex].objectIndex >= topology.objects.size()
            || row.targetObjectKey
                   != topology.objects[topology.slots[row.objectiveSlotIndex].objectIndex].objectKey
            || row.bitIndex >= 24U || row.flags != format::kTaskTargetExact || row.reserved != 0
            || (index != 0 && !task_less(snapshot.taskTargets[index - 1], row))) {
            return false;
        }
    }
    for (std::size_t index = 1; index < snapshot.taskTargets.size(); ++index) {
        if (task_natural(snapshot.taskTargets[index - 1])
            == task_natural(snapshot.taskTargets[index])) {
            return false;
        }
    }
    return true;
}

/** Builds both authored-scene sections from complete topology and package facts. */
bool build(const topology::Snapshot& topology,
           const Facts& facts,
           squad::TagReader reader,
           void* readerContext,
           Snapshot& output) noexcept {
    output = {};
    if (reader == nullptr || !valid_topology(topology)) {
        return false;
    }
    try {
        SchemaIndex schemas{};
        PackageCache cache{};
        Snapshot pending{};
        if (!schema_index(topology, facts, schemas)) {
            return false;
        }
        const std::size_t sceneDescriptorCount = static_cast<std::size_t>(std::count_if(
            facts.descriptors.begin(), facts.descriptors.end(), [&topology](const auto& row) {
                return is_scene_descriptor(topology, row);
            }));
        cache.reserve(sceneDescriptorCount);
        pending.resources.reserve(sceneDescriptorCount);
        pending.squadEdges.reserve(sceneDescriptorCount);
        pending.taskTargets.reserve(facts.descriptors.size() - sceneDescriptorCount);
        for (const squad::DescriptorFact& descriptor : facts.descriptors) {
            if (is_task_descriptor(topology, descriptor)) {
                if (!slot_shape(topology,
                                schemas,
                                descriptor.slotIndex,
                                format::kTaskSlotType,
                                format::kTaskComponentClass,
                                format::kAbsentIndex,
                                format::kTaskAuthSchema)) {
                    continue;
                }
                const PackageRow* config = nullptr;
                if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                    || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                    continue;
                }
                const auto blob = std::span(config->bytes);
                const std::size_t referenceField =
                    static_cast<std::size_t>(descriptor.descriptorOffset)
                    + format::kTaskReferenceRelativeOffset;
                const std::size_t bitIndexField =
                    static_cast<std::size_t>(descriptor.descriptorOffset)
                    + format::kTaskBitIndexRelativeOffset;
                std::uint32_t targetObjectKey = 0;
                std::uint16_t targetSlotType = 0;
                std::uint16_t targetSlotIndex = 0;
                std::uint32_t bitIndex = 0;
                if (!read_value(blob, referenceField, targetObjectKey)
                    || !read_value(
                        blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                    || !read_value(
                        blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)
                    || !read_value(blob, bitIndexField, bitIndex)) {
                    return false;
                }
                if (targetSlotType == format::kObjectiveSlotType && bitIndex < 24U) {
                    std::uint32_t linkedSlot = format::kAbsentIndex;
                    bool found = false;
                    if (!target_slot(topology,
                                     descriptor,
                                     targetObjectKey,
                                     targetSlotType,
                                     targetSlotIndex,
                                     linkedSlot,
                                     found)) {
                        return false;
                    }
                    if (found
                        && slot_shape(topology,
                                      schemas,
                                      linkedSlot,
                                      format::kObjectiveSlotType,
                                      format::kObjectiveComponentClass,
                                      format::kObjectiveSenseSchema,
                                      format::kObjectiveAuthSchema)) {
                        TaskTarget row{};
                        if (!task_target_id(topology, descriptor, row.id)) {
                            continue;
                        }
                        row.taskSlotIndex = descriptor.slotIndex;
                        row.objectiveSlotIndex = linkedSlot;
                        row.configTag = descriptor.configTag;
                        row.descriptorOffset = descriptor.descriptorOffset;
                        row.referenceFieldOffset = static_cast<std::uint32_t>(referenceField);
                        row.targetObjectKey = targetObjectKey;
                        row.bitIndex = bitIndex;
                        row.flags = format::kTaskTargetExact;
                        pending.taskTargets.push_back(row);
                    }
                }
            }
            if (is_performance_descriptor(topology, descriptor)) {
                // A type-42 sensor drives the same-object squad named at descriptor +0x58.
                if (!slot_shape(topology,
                                schemas,
                                descriptor.slotIndex,
                                format::kPerformanceSlotType,
                                format::kPerformanceComponentClass,
                                format::kAbsentIndex,
                                format::kPerformanceAuthSchema)) {
                    log_performance_edge(descriptor, "sensor_shape");
                    continue;
                }
                const PackageRow* config = nullptr;
                if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                    || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                    log_performance_edge(descriptor, "config_unreadable");
                    continue;
                }
                const auto blob = std::span(config->bytes);
                const std::size_t referenceField =
                    static_cast<std::size_t>(descriptor.descriptorOffset)
                    + format::kPerformanceSquadReferenceRelativeOffset;
                std::uint32_t targetObjectKey = 0;
                std::uint16_t targetSlotType = 0;
                std::uint16_t targetSlotIndex = 0;
                if (!read_value(blob, referenceField, targetObjectKey)
                    || !read_value(
                        blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                    || !read_value(
                        blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)
                    || targetSlotType != format::kSquadSlotType
                    || descriptor.objectIndex >= topology.objects.size()
                    || targetObjectKey != topology.objects[descriptor.objectIndex].objectKey) {
                    log_performance_edge(descriptor, "reference");
                    continue;
                }
                std::uint32_t linkedSlot = format::kAbsentIndex;
                if (!same_object_slot(
                        topology, descriptor, targetSlotType, targetSlotIndex, linkedSlot)) {
                    return false;
                }
                if (linkedSlot == format::kAbsentIndex
                    || !slot_shape(topology,
                                   schemas,
                                   linkedSlot,
                                   format::kSquadSlotType,
                                   format::kSquadComponentClass,
                                   format::kSquadSenseSchema,
                                   format::kSquadAuthSchema)) {
                    log_performance_edge(descriptor, "target_shape");
                    continue;
                }
                SquadEdge row{};
                if (!edge_id(topology, descriptor, row.id)) {
                    log_performance_edge(descriptor, "identity");
                    continue;
                }
                row.sceneSlotIndex = descriptor.slotIndex;
                row.squadSlotIndex = linkedSlot;
                row.configTag = descriptor.configTag;
                row.descriptorOffset = descriptor.descriptorOffset;
                row.referenceFieldOffset = static_cast<std::uint32_t>(referenceField);
                row.targetObjectKey = targetObjectKey;
                row.flags = format::kAuthoredSceneSquadPerformanceTargetExact;
                pending.squadEdges.push_back(row);
                log_performance_edge(descriptor, "ready");
                continue;
            }
            if (!is_scene_descriptor(topology, descriptor)) {
                continue;
            }
            if (!slot_shape(topology,
                            schemas,
                            descriptor.slotIndex,
                            format::kAuthoredSceneSlotType,
                            format::kAuthoredSceneComponentClass,
                            format::kAuthoredSceneSenseSchema,
                            format::kAuthoredSceneAuthSchema)) {
                continue;
            }
            const PackageRow* config = nullptr;
            if (!package_row(reader, readerContext, descriptor.configTag, cache, config)
                || config == nullptr || config->classId != tables::kPlacedObjectClass) {
                continue;
            }
            const auto blob = std::span(config->bytes);
            const std::size_t resourceField = static_cast<std::size_t>(descriptor.descriptorOffset)
                                              + format::kAuthoredSceneResourceRelativeOffset;
            std::uint32_t resourceTag = 0;
            if (!read_value(blob, resourceField, resourceTag)) {
                continue;
            }
            if (resourceTag != 0 && resourceTag != format::kAbsentIndex) {
                const PackageRow* resourcePackage = nullptr;
                if (package_row(reader, readerContext, resourceTag, cache, resourcePackage)
                    && resourcePackage != nullptr
                    && resourcePackage->classId == format::kAuthoredSceneResourceClass) {
                    Resource row{};
                    if (resource_id(topology, descriptor, row.id)) {
                        row.slotIndex = descriptor.slotIndex;
                        row.configTag = descriptor.configTag;
                        row.descriptorOffset = descriptor.descriptorOffset;
                        row.resourceFieldOffset = static_cast<std::uint32_t>(resourceField);
                        row.resourceTag = resourceTag;
                        row.resourceClass = resourcePackage->classId;
                        row.flags = format::kAuthoredSceneResourceExact;
                        pending.resources.push_back(row);
                    }
                }
            }

            const std::size_t blockClassField =
                static_cast<std::size_t>(descriptor.descriptorOffset)
                + format::kAuthoredSceneSquadBlockClassRelativeOffset;
            std::uint32_t blockClass = 0;
            if (!read_value(blob, blockClassField, blockClass)) {
                continue;
            }
            if (blockClass != format::kAuthoredSceneSquadBlockClass) {
                continue;
            }
            const std::size_t referenceField = static_cast<std::size_t>(descriptor.descriptorOffset)
                                               + format::kAuthoredSceneSquadReferenceRelativeOffset;
            std::uint32_t targetObjectKey = 0;
            std::uint16_t targetSlotType = 0;
            std::uint16_t targetSlotIndex = 0;
            if (!read_value(blob, referenceField, targetObjectKey)
                || !read_value(blob, referenceField + kTargetSlotTypeRelativeOffset, targetSlotType)
                || !read_value(
                    blob, referenceField + kTargetSlotIndexRelativeOffset, targetSlotIndex)) {
                continue;
            }
            if (targetSlotType != format::kSquadSlotType) {
                continue;
            }
            if (descriptor.objectIndex >= topology.objects.size()
                || targetObjectKey != topology.objects[descriptor.objectIndex].objectKey) {
                continue;
            }
            std::uint32_t linkedSlot = format::kAbsentIndex;
            if (!same_object_slot(
                    topology, descriptor, targetSlotType, targetSlotIndex, linkedSlot)) {
                return false;
            }
            if (linkedSlot == format::kAbsentIndex
                || !slot_shape(topology,
                               schemas,
                               linkedSlot,
                               format::kSquadSlotType,
                               format::kSquadComponentClass,
                               format::kSquadSenseSchema,
                               format::kSquadAuthSchema)) {
                continue;
            }
            SquadEdge row{};
            if (!edge_id(topology, descriptor, row.id)) {
                continue;
            }
            row.sceneSlotIndex = descriptor.slotIndex;
            row.squadSlotIndex = linkedSlot;
            row.configTag = descriptor.configTag;
            row.descriptorOffset = descriptor.descriptorOffset;
            row.referenceFieldOffset = static_cast<std::uint32_t>(referenceField);
            row.targetObjectKey = targetObjectKey;
            row.flags = format::kAuthoredSceneSquadSameObjectExact;
            pending.squadEdges.push_back(row);
        }
        // A sensor with no descriptor fact logs nothing above, so count both sides here.
        {
            const std::size_t sensorSlots = static_cast<std::size_t>(
                std::count_if(topology.slots.begin(), topology.slots.end(), [](const auto& slot) {
                    return slot.slotType == format::kPerformanceSlotType;
                }));
            const std::size_t sensorDescriptors = static_cast<std::size_t>(std::count_if(
                facts.descriptors.begin(), facts.descriptors.end(), [&topology](const auto& row) {
                    return is_performance_descriptor(topology, row);
                }));
            std::array<char, 160> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=activity_sdk_performance_edge stage=summary "
                                              "sensor_slots=%zu sensor_descriptors=%zu",
                                              sensorSlots,
                                              sensorDescriptors);
            if (written > 0) {
                core::log::write(
                    core::log::Channel::client,
                    core::log::Level::info,
                    {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
            }
        }
        std::sort(pending.resources.begin(), pending.resources.end(), resource_less);
        std::sort(pending.squadEdges.begin(), pending.squadEdges.end(), edge_less);
        std::sort(pending.taskTargets.begin(), pending.taskTargets.end(), task_less);
        pending.resources.erase(std::unique(pending.resources.begin(),
                                            pending.resources.end(),
                                            [](const auto& left, const auto& right) {
                                                return resource_natural(left)
                                                       == resource_natural(right);
                                            }),
                                pending.resources.end());
        pending.squadEdges.erase(std::unique(pending.squadEdges.begin(),
                                             pending.squadEdges.end(),
                                             [](const auto& left, const auto& right) {
                                                 return edge_natural(left) == edge_natural(right);
                                             }),
                                 pending.squadEdges.end());
        pending.taskTargets.erase(std::unique(pending.taskTargets.begin(),
                                              pending.taskTargets.end(),
                                              [](const auto& left, const auto& right) {
                                                  return task_natural(left) == task_natural(right);
                                              }),
                                  pending.taskTargets.end());
        pending.complete = true;
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
