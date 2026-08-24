#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../state/build_data/scenarios/scenario_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::placed_objects {

namespace layouts = state::build_data::scenarios;
namespace tables = middleware::content::packages::tables;

/** Slot rows shown for one package-backed roster object before the preview is truncated. */
inline constexpr std::size_t kSlotPreviewCapacity = 16;
static_assert(kSlotPreviewCapacity <= layouts::kRosterSlotCapacity);

/** One factual slot descriptor retained from the scenario roster catalog. */
struct SlotSnapshot final {
    std::uint16_t index{};
    std::uint8_t type{};
    std::uint8_t flags{};

    [[nodiscard]] friend bool operator==(const SlotSnapshot&,
                                         const SlotSnapshot&) noexcept = default;
};

/**
 * One package-backed object reached by the current destination's published roster groups.
 * This is authored placement metadata, not proof that a live object or SimEntity exists.
 */
struct Snapshot final {
    std::uint16_t groupIndex{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotCount{};
    std::uint8_t slotPreviewCount{};
    bool slotsTruncated{};
    std::array<SlotSnapshot, kSlotPreviewCapacity> slots{};

    [[nodiscard]] friend bool operator==(const Snapshot&, const Snapshot&) noexcept = default;
};

/** Summary returned after placement nodes are appended to an inspection graph. */
struct AppendResult final {
    NodeId groupNode{};
    std::size_t objectCount{};
    std::size_t slotCount{};
    bool slotsTruncated{};
};

[[nodiscard]] inline const char* slot_authority_label(std::uint8_t flags) noexcept {
    const bool auth = (flags & layouts::kSlotAuthFlag) != 0;
    const bool sense = (flags & layouts::kSlotSenseFlag) != 0;
    if (auth && sense) {
        return "auth+sense";
    }
    if (auth) {
        return "auth";
    }
    if (sense) {
        return "sense";
    }
    return "content";
}

[[nodiscard]] inline std::string object_label(const Snapshot& object) {
    std::array<char, 128> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "Placed object 0x%08X / key 0x%08X / %u slots",
                                      object.objectTag,
                                      object.registryKey,
                                      static_cast<unsigned>(object.slotCount));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Placed roster object");
}

[[nodiscard]] inline std::string object_search(const Snapshot& object) {
    std::array<char, 256> header{};
    const int written =
        std::snprintf(header.data(),
                      header.size(),
                      "package-backed authored placed roster object partial-entity group=%u "
                      "object-tag=0x%08X registry-key=0x%08X slots=%u",
                      static_cast<unsigned>(object.groupIndex),
                      object.objectTag,
                      object.registryKey,
                      static_cast<unsigned>(object.slotCount));
    std::string result = written > 0 && static_cast<std::size_t>(written) < header.size()
                             ? std::string(header.data(), static_cast<std::size_t>(written))
                             : std::string("package-backed placed roster object");
    for (std::size_t index = 0; index < object.slotPreviewCount; ++index) {
        const SlotSnapshot& slot = object.slots[index];
        std::array<char, 96> token{};
        const int tokenWritten = std::snprintf(token.data(),
                                               token.size(),
                                               " slot-index=%u slot-type=%u slot-mode=%s",
                                               static_cast<unsigned>(slot.index),
                                               static_cast<unsigned>(slot.type),
                                               slot_authority_label(slot.flags));
        if (tokenWritten > 0 && static_cast<std::size_t>(tokenWritten) < token.size()) {
            result.append(token.data(), static_cast<std::size_t>(tokenWritten));
        }
    }
    return result;
}

[[nodiscard]] inline std::string slot_label(const SlotSnapshot& slot) {
    std::array<char, 96> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "Slot %u / type %u / %s",
                                      static_cast<unsigned>(slot.index),
                                      static_cast<unsigned>(slot.type),
                                      slot_authority_label(slot.flags));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string("Component slot");
}

inline void append_group_index(std::span<std::uint16_t> indices,
                               std::size_t& count,
                               std::uint16_t groupIndex) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (indices[index] == groupIndex) {
            return;
        }
    }
    if (count < indices.size()) {
        indices[count++] = groupIndex;
    }
}

/**
 * Captures destination-wide groups plus groups whose bubble mask includes the current bubble.
 * The copied rows make graph construction coherent even if the backing catalog later changes.
 */
inline void
collect(const layouts::Definition& layout, std::int32_t bubble, std::vector<Snapshot>& output) {
    constexpr std::size_t kGroupIndexCapacity =
        layouts::kDestinationGroupCapacity + layouts::kDestinationBubbleGroupCapacity;
    std::array<std::uint16_t, kGroupIndexCapacity> indices{};
    std::size_t indexCount = 0;
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        append_group_index(indices, indexCount, layout.rosterGroups[index]);
    }
    if (bubble >= 0 && static_cast<std::size_t>(bubble) < layouts::kBubbleCapacity) {
        const std::uint64_t bubbleBit = std::uint64_t{1} << static_cast<unsigned>(bubble);
        for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
            if ((layout.bubbleGroupMasks[index] & bubbleBit) != 0) {
                append_group_index(indices, indexCount, layout.bubbleGroups[index]);
            }
        }
    }

    output.clear();
    output.reserve(indexCount);
    for (std::size_t index = 0; index < indexCount; ++index) {
        layouts::RosterGroup group{};
        if (!layouts::group(indices[index], group)) {
            continue;
        }
        Snapshot object{};
        object.groupIndex = indices[index];
        object.registryKey = group.registryKey;
        object.objectTag = group.objectTag;
        object.slotCount = group.slotCount;
        const std::size_t previewCount =
            (std::min)(static_cast<std::size_t>(group.slotCount), kSlotPreviewCapacity);
        object.slotPreviewCount = static_cast<std::uint8_t>(previewCount);
        object.slotsTruncated = previewCount < group.slotCount;
        for (std::size_t slot = 0; slot < previewCount; ++slot) {
            object.slots[slot] = {
                group.slotIndices[slot], group.slotTypes[slot], group.slotFlags[slot]};
        }
        output.push_back(object);
    }
    std::sort(
        output.begin(), output.end(), [](const Snapshot& left, const Snapshot& right) noexcept {
            if (left.objectTag != right.objectTag) {
                return left.objectTag < right.objectTag;
            }
            if (left.registryKey != right.registryKey) {
                return left.registryKey < right.registryKey;
            }
            return left.groupIndex < right.groupIndex;
        });
}

/** Appends factual placement and component-slot rows without inventing runtime identities. */
[[nodiscard]] inline AppendResult append(Graph& graph,
                                         std::vector<Diagnostic>& diagnostics,
                                         std::span<const Snapshot> objects,
                                         const Source& source,
                                         NodeId parent) {
    AppendResult result{};
    if (objects.empty()) {
        return result;
    }

    Node groupNode;
    std::array<char, 96> groupLabel{};
    const int groupWritten = std::snprintf(groupLabel.data(),
                                           groupLabel.size(),
                                           "Package-backed placed objects (%zu)",
                                           objects.size());
    groupNode.name = groupWritten > 0 && static_cast<std::size_t>(groupWritten) < groupLabel.size()
                         ? std::string(groupLabel.data(), static_cast<std::size_t>(groupWritten))
                         : std::string("Package-backed placed objects");
    groupNode.searchText =
        "partial entity coverage authored scenario roster package-backed placed objects";
    groupNode.kind = NodeKind::placedObject;
    groupNode.status = Status::known;
    groupNode.producer = Producer::catalog;
    groupNode.provenance = Provenance::catalog;
    groupNode.source = source;
    groupNode.actions = Action::copyId;
    result.groupNode = graph.add(std::move(groupNode), parent);
    if (!result.groupNode) {
        diagnostics.push_back({Diagnostic::Severity::error,
                               "The inspection graph could not create its placed-object group."});
        return result;
    }

    for (const Snapshot& object : objects) {
        const std::string objectSearch = object_search(object);
        const std::uint64_t objectNativeKey =
            (static_cast<std::uint64_t>(object.objectTag) << 32U) | object.registryKey;
        Node objectNode;
        objectNode.name = object_label(object);
        objectNode.searchText = objectSearch;
        objectNode.kind = NodeKind::placedObject;
        objectNode.status = Status::unknownSemantic;
        objectNode.producer = Producer::catalog;
        objectNode.provenance = Provenance::catalog;
        objectNode.nativeKey = objectNativeKey;
        objectNode.source = source;
        objectNode.tag = object.objectTag;
        objectNode.classHash = tables::kObjectClass;
        objectNode.actions = Action::copyId | Action::copyTag;
        const NodeId objectId = graph.add(std::move(objectNode), result.groupNode);
        if (!objectId) {
            diagnostics.push_back(
                {Diagnostic::Severity::error,
                 "The inspection graph reached its node-id capacity while adding placed objects."});
            break;
        }

        ++result.objectCount;
        result.slotCount += object.slotCount;
        result.slotsTruncated = result.slotsTruncated || object.slotsTruncated;
        for (std::size_t slot = 0; slot < object.slotPreviewCount; ++slot) {
            const SlotSnapshot& value = object.slots[slot];
            Node slotNode;
            slotNode.name = slot_label(value);
            slotNode.searchText = objectSearch;
            slotNode.searchText.append(" component slot ");
            slotNode.searchText.append(slotNode.name);
            slotNode.kind = NodeKind::componentSlot;
            slotNode.status = Status::known;
            slotNode.producer = Producer::catalog;
            slotNode.provenance = Provenance::catalog;
            slotNode.nativeKey = objectNativeKey
                                 ^ (0x9E3779B97F4A7C15ULL + value.index + (objectNativeKey << 6U)
                                    + (objectNativeKey >> 2U));
            slotNode.source = source;
            slotNode.actions = Action::copyId;
            if (!graph.add(std::move(slotNode), objectId)) {
                diagnostics.push_back(
                    {Diagnostic::Severity::error,
                     "The inspection graph reached its node-id capacity while adding slot rows."});
                break;
            }
        }
    }

    std::array<char, 256> coverage{};
    const int coverageWritten = std::snprintf(
        coverage.data(),
        coverage.size(),
        "Package-backed placement coverage: %zu roster objects with %zu declared component slots.",
        result.objectCount,
        result.slotCount);
    if (coverageWritten > 0 && static_cast<std::size_t>(coverageWritten) < coverage.size()) {
        diagnostics.push_back(
            {Diagnostic::Severity::information,
             std::string(coverage.data(), static_cast<std::size_t>(coverageWritten))});
    }
    if (result.slotsTruncated) {
        diagnostics.push_back(
            {Diagnostic::Severity::information,
             "Placed-object slot children are a bounded preview; each object row retains its full "
             "declared slot count."});
    }
    return result;
}

} // namespace sunrise::client::inspection::providers::placed_objects
