#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Reports where the node pass stopped, so a silent miss cannot look like a stuck progress bar. */
void report(const char* stage, unsigned long long detail) noexcept {
    std::array<char, 128> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=pkg stage=nodes result=%s detail=%llu", stage, detail);
    if (count > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Reads a value slot out of one node expression, or reports that it names none. */
[[nodiscard]] bool expression_value_slot(std::span<const std::byte> table,
                                         std::size_t rowAt,
                                         std::size_t field,
                                         std::int16_t& slot) noexcept {
    std::int64_t count = 0;
    std::int64_t relative = 0;
    std::memcpy(&count, table.data() + rowAt + field, sizeof count);
    std::memcpy(&relative, table.data() + rowAt + field + 8, sizeof relative);
    if (count < 1 || count > tables::kNodeExpressionCapacity) {
        return false;
    }
    const std::size_t pointerAt = rowAt + field + 8;
    const std::int64_t target = static_cast<std::int64_t>(pointerAt) + relative
                                + static_cast<std::int64_t>(tables::kHeaderSkip);
    if (target < 0
        || static_cast<std::size_t>(target)
                   + static_cast<std::size_t>(count) * tables::kUnlockInstructionStride
               > table.size()) {
        return false;
    }
    const auto base = static_cast<std::size_t>(target);
    for (std::int64_t index = 0; index < count; ++index) {
        std::uint32_t opcode = 0;
        std::uint32_t operand = 0;
        const std::size_t at =
            base + static_cast<std::size_t>(index) * tables::kUnlockInstructionStride;
        std::memcpy(&opcode, table.data() + at, sizeof opcode);
        std::memcpy(&operand, table.data() + at + 4, sizeof operand);
        if (opcode > tables::kUnlockOpcodeCeiling) {
            return false;
        }
        // Opcode ten reads a value, which is the slot a node's progress bar shows.
        if (opcode == tables::kUnlockReadValueOpcode
            && operand <= static_cast<std::uint32_t>(INT16_MAX)) {
            slot = static_cast<std::int16_t>(operand);
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * Reads the presentation node table and resolves each node's value slot and owned records.
 *
 * A node's progress bar shows a value slot named by its own expression, and the records it owns sit
 * at row `+136` as a row and a gate. Both are read here so a claim never has to walk the node table.
 */
bool build_nodes(const reader::Source& source,
                 reader::Scratch& scratch,
                 std::span<const std::byte> root,
                 std::vector<std::byte>& blob,
                 std::span<state::build_data::nodes::Definition> output,
                 std::size_t& count) noexcept {
    namespace domain = state::build_data::nodes;
    count = 0;

    std::uint32_t mapTag = 0;
    tables::Array mapRows{};
    if (!tables::slot_tag(root, tables::kUnlockValueMapTableSlot, mapTag) || mapTag == 0
        || tables::package_of(mapTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, scratch, mapTag, blob)
        || !tables::find_array_at(std::span<const std::byte>{blob},
                                  tables::kAccountValueMapDescriptor,
                                  mapRows)
        || mapRows.count == 0
        || mapRows.dataOffset
                   + static_cast<std::size_t>(mapRows.count) * tables::kUnlockMapRowStride
               > blob.size()) {
        report("value_map_fail", mapTag);
        return false;
    }
    std::unordered_map<std::int16_t, std::uint16_t> indexBySlot{};
    for (std::uint64_t row = 0; row < mapRows.count && row <= domain::kUnavailableValueIndex;
         ++row) {
        const std::size_t at =
            mapRows.dataOffset + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride;
        std::int16_t slot = 0;
        std::memcpy(&slot, blob.data() + at + tables::kUnlockMapDestinationSlotOffset, sizeof slot);
        indexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
    }

    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kPresentationNodeTableSlot, tableTag) || tableTag == 0
        || tables::package_of(tableTag) == tables::kAbsentPackageId
        || !reader::read_tag(source, scratch, tableTag, blob)
        || !tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kTableArrayDescriptor, rows)
        || rows.count == 0 || rows.count > output.size()
        || rows.dataOffset + static_cast<std::size_t>(rows.count) * tables::kNodeRowStride
               > blob.size()) {
        report("node_table_fail", tableTag);
        return false;
    }

    const std::span<const std::byte> table{blob};
    std::size_t driving = 0;
    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kNodeRowStride;
        domain::Definition& definition = output[static_cast<std::size_t>(row)];
        definition = {};
        definition.definitionIndex = static_cast<std::uint16_t>(row);

        // The expression sits at one of two fields, and only one of them holds it on any node.
        std::int16_t slot = 0;
        const bool named =
            expression_value_slot(table, at, tables::kNodeExpressionFieldPrimary, slot)
            || expression_value_slot(table, at, tables::kNodeExpressionFieldAlternate, slot);
        if (named) {
            const auto found = indexBySlot.find(slot);
            if (found != indexBySlot.end()) {
                definition.valueIndex = found->second;
            }
        }

        // Records the node owns, four bytes each as a row and a gate.
        std::int64_t childCount = 0;
        std::int64_t childRelative = 0;
        std::memcpy(&childCount, table.data() + at + tables::kNodeChildRecordField,
                    sizeof childCount);
        std::memcpy(&childRelative, table.data() + at + tables::kNodeChildRecordField + 8,
                    sizeof childRelative);
        if (childCount >= 1 && childCount <= static_cast<std::int64_t>(domain::kChildCapacity)) {
            const std::size_t pointerAt = at + tables::kNodeChildRecordField + 8;
            const std::int64_t target = static_cast<std::int64_t>(pointerAt) + childRelative
                                        + static_cast<std::int64_t>(tables::kHeaderSkip);
            if (target >= 0
                && static_cast<std::size_t>(target)
                           + static_cast<std::size_t>(childCount) * tables::kNodeChildRecordStride
                       <= table.size()) {
                const auto base = static_cast<std::size_t>(target);
                for (std::int64_t index = 0; index < childCount; ++index) {
                    std::uint16_t childRow = 0;
                    std::memcpy(&childRow,
                                table.data() + base
                                    + static_cast<std::size_t>(index)
                                          * tables::kNodeChildRecordStride,
                                sizeof childRow);
                    definition.children[static_cast<std::size_t>(definition.childCount++)] =
                        childRow;
                }
            }
        }
        if (definition.childCount != 0 && definition.valueIndex != domain::kUnavailableValueIndex) {
            ++driving;
        }
        ++count;
    }
    report("ok", static_cast<unsigned long long>(driving));
    return count != 0;
}

} // namespace sunrise::client::content::items::packages
