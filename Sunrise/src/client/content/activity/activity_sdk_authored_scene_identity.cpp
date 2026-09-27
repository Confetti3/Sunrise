#include "activity_sdk_authored_scene_internal.h"
#include "activity_sdk_authored_scene_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory {

/** Formats one resource ID from its exact descriptor tuple. */
bool resource_id(const topology::Snapshot& topology,
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

/** Formats one event-key ID from the resource, its graph, the scene slot and the gate. */
bool event_key_id(const topology::Snapshot& topology,
                  const squad::DescriptorFact& descriptor,
                  std::uint32_t graphTag,
                  std::uint32_t gateOffset,
                  Text& output) noexcept {
    if (descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    return format_text(output,
                       "authored-scene-event-key/%08x/%08x/%04x/%04x/%08x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(graphTag),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType),
                       static_cast<unsigned>(gateOffset));
}

/** Formats one scene-to-squad edge ID from its descriptor tuple and the squad slot it names. */
bool edge_id(const topology::Snapshot& topology,
             const squad::DescriptorFact& descriptor,
             std::uint32_t squadSlotRow,
             Text& output) noexcept {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size() || squadSlotRow >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    const topology::Slot& squad = topology.slots[squadSlotRow];
    return format_text(output,
                       "authored-scene-squad-edge/%08x/%08x/%08x/%04x/%04x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType),
                       static_cast<unsigned>(squad.slotIndex),
                       static_cast<unsigned>(squad.slotType));
}

/** Formats one task-to-objective target ID from its exact descriptor tuple. */
bool task_target_id(const topology::Snapshot& topology,
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

/** Tests one exact final slot shape supplied by the separate schema join. */
bool slot_shape(const topology::Snapshot& topology,
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

/** Builds and validates the unique global schema lookup. */
bool schema_index(const topology::Snapshot& topology, const Facts& facts, SchemaIndex& output) {
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
bool valid_topology(const topology::Snapshot& topology) noexcept {
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

} // namespace sunrise::client::content::activity::sdk_generation::authored_scene_inventory
