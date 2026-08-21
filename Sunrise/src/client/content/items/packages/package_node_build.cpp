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

/** Reads the flag slot one expression tests, or reports that it tests none. */
[[nodiscard]] bool expression_flag_slot(std::span<const std::byte> table,
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
        if (opcode == tables::kUnlockReadFlagOpcode
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

    // The account flag mapping table, read first. A category's gate names a flag slot, and a slot
    // is not an index: the byte that feeds it sits at the row whose destination is that slot.
    std::uint32_t flagMapTag = 0;
    tables::Array flagMapRows{};
    std::unordered_map<std::int16_t, std::uint16_t> flagIndexBySlot{};
    if (tables::slot_tag(root, tables::kUnlockFlagMapTableSlot, flagMapTag) && flagMapTag != 0
        && tables::package_of(flagMapTag) != tables::kAbsentPackageId
        && reader::read_tag(source, scratch, flagMapTag, blob)
        && tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kAccountFlagMapDescriptor, flagMapRows)
        && flagMapRows.count != 0
        && flagMapRows.dataOffset
                   + static_cast<std::size_t>(flagMapRows.count) * tables::kUnlockMapRowStride
               <= blob.size()) {
        for (std::uint64_t row = 0; row < flagMapRows.count && row <= domain::kUnavailableFlagIndex;
             ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + flagMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            flagIndexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
        }
    }

    // TEMPORARY PROBE: one lore gate resolves to no account row. Root slot 111 holds several flag
    // mapping tables and only the account one has been read, so the gate is likely in another scope
    // rather than absent. Enumerate every descriptor that yields a table and say where 14907 lands.
    for (std::size_t descriptor = 0; descriptor <= 72; descriptor += 8) {
        tables::Array probeRows{};
        if (!tables::find_array_at(std::span<const std::byte>{blob}, descriptor, probeRows)
            || probeRows.count == 0
            || probeRows.dataOffset
                       + static_cast<std::size_t>(probeRows.count) * tables::kUnlockMapRowStride
                   > blob.size()) {
            continue;
        }
        int found = -1;
        for (std::uint64_t row = 0; row < probeRows.count; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + probeRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            if (slot == static_cast<std::int16_t>(14907)) {
                found = static_cast<int>(row);
                break;
            }
        }
        std::array<char, 200> line{};
        const int told = std::snprintf(line.data(), line.size(),
                                       "ev=fmapscope descriptor=%zu rows=%llu slot14907_index=%d",
                                       descriptor,
                                       static_cast<unsigned long long>(probeRows.count), found);
        if (told > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(told)});
        }
    }

    tables::Array characterFlagMapRows{};
    std::unordered_map<std::int16_t, std::uint16_t> characterFlagIndexBySlot{};
    if (flagMapTag != 0
        && tables::find_array_at(std::span<const std::byte>{blob},
                                 tables::kCharacterFlagMapDescriptor,
                                 characterFlagMapRows)
        && characterFlagMapRows.count != 0
        && characterFlagMapRows.dataOffset
                   + static_cast<std::size_t>(characterFlagMapRows.count)
                         * tables::kUnlockMapRowStride
               <= blob.size()) {
        for (std::uint64_t row = 0;
             row < characterFlagMapRows.count && row <= domain::kUnavailableFlagIndex; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + characterFlagMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            characterFlagIndexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
        }
    }

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

    // TEMPORARY PROBE: one lore book's gate reads a value slot that the account map does not
    // carry, so it is scoped elsewhere. Say which table holds it.
    for (std::size_t descriptor = 0; descriptor <= 72; descriptor += 8) {
        tables::Array probeRows{};
        if (!tables::find_array_at(std::span<const std::byte>{blob}, descriptor, probeRows)
            || probeRows.count == 0
            || probeRows.dataOffset
                       + static_cast<std::size_t>(probeRows.count) * tables::kUnlockMapRowStride
                   > blob.size()) {
            continue;
        }
        int found = -1;
        for (std::uint64_t row = 0; row < probeRows.count; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + probeRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            if (slot == static_cast<std::int16_t>(8275)) {
                found = static_cast<int>(row);
                break;
            }
        }
        std::array<char, 200> line{};
        const int told = std::snprintf(line.data(), line.size(),
                                       "ev=vmapscope descriptor=%zu rows=%llu slot8275_index=%d",
                                       descriptor,
                                       static_cast<unsigned long long>(probeRows.count), found);
        if (told > 0) {
            core::log::write(core::log::Channel::client, core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(told)});
        }
    }

    tables::Array characterValueMapRows{};
    std::unordered_map<std::int16_t, std::uint16_t> characterValueIndexBySlot{};
    if (tables::find_array_at(std::span<const std::byte>{blob},
                              tables::kCharacterValueMapDescriptor,
                              characterValueMapRows)
        && characterValueMapRows.count != 0
        && characterValueMapRows.dataOffset
                   + static_cast<std::size_t>(characterValueMapRows.count)
                         * tables::kUnlockMapRowStride
               <= blob.size()) {
        for (std::uint64_t row = 0;
             row < characterValueMapRows.count && row <= domain::kUnavailableValueIndex; ++row) {
            std::int16_t slot = 0;
            std::memcpy(&slot,
                        blob.data() + characterValueMapRows.dataOffset
                            + static_cast<std::size_t>(row) * tables::kUnlockMapRowStride
                            + tables::kUnlockMapDestinationSlotOffset,
                        sizeof slot);
            characterValueIndexBySlot.emplace(slot, static_cast<std::uint16_t>(row));
        }
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
            // The parent record's own bar reads the next slot up. Resolve it through the mapping
            // table rather than adding one to the index: rows happen to run in slot order around
            // here, but nothing guarantees that.
            const auto characterSlot = characterValueIndexBySlot.find(slot);
            if (characterSlot != characterValueIndexBySlot.end()) {
                definition.characterValueIndex = characterSlot->second;
            }
            const auto parent = indexBySlot.find(
                static_cast<std::int16_t>(slot + tables::kNodeParentSlotStep));
            if (parent != indexBySlot.end()) {
                definition.parentValueIndex = parent->second;
            }
        }

        // A category gated on a flag rather than on its own progress cannot reveal itself by being
        // played: with no title shown there is nothing inside to claim, and nothing to claim leaves
        // the gate shut. Resolve that flag so the gate can be satisfied.
        std::int16_t gateSlot = 0;
        if (expression_flag_slot(table, at, tables::kNodeExpressionFieldPrimary, gateSlot)
            || expression_flag_slot(table, at, tables::kNodeExpressionFieldAlternate, gateSlot)) {
            const auto gate = flagIndexBySlot.find(gateSlot);
            if (gate != flagIndexBySlot.end()) {
                definition.visibilityFlagIndex = gate->second;
            }
            const auto characterGate = characterFlagIndexBySlot.find(gateSlot);
            if (characterGate != characterFlagIndexBySlot.end()) {
                definition.visibilityCharacterFlagIndex = characterGate->second;
            }
        }

        // TEMPORARY PROBE: sweep the whole row, every four byte offset, not a hand written list
        // of fields. The previous sweep covered sixteen to one hundred and twenty eight in steps of
        // eight and so never looked at the first two fields or anything past the child array, which
        // is most of the reason a gate could still be sitting somewhere unread.
        if (row == 819 || row == 838) {
            for (std::size_t field = 0; field + 16 <= tables::kNodeRowStride; field += 4) {
                std::int64_t count = 0;
                std::int64_t relative = 0;
                std::memcpy(&count, table.data() + at + field, sizeof count);
                std::memcpy(&relative, table.data() + at + field + 8, sizeof relative);
                if (count < 1 || count > 4096 || relative == 0) {
                    continue;
                }
                const std::size_t pointerAt = at + field + 8;
                const std::int64_t target = static_cast<std::int64_t>(pointerAt) + relative
                                            + static_cast<std::int64_t>(tables::kHeaderSkip);
                if (target < 0
                    || static_cast<std::size_t>(target)
                               + static_cast<std::size_t>(count) * tables::kUnlockInstructionStride
                           > table.size()) {
                    continue;
                }

                // Print the stream itself. A field carrying a plausible count and a target inside
                // the table is worth reading whatever its offset, and the opcodes say at once
                // whether it is an expression or an array being misread as one.
                const auto base = static_cast<std::size_t>(target);
                const std::int64_t shown = count < 8 ? count : 8;
                for (std::int64_t step = 0; step < shown; ++step) {
                    std::uint32_t opcode = 0;
                    std::uint32_t operand = 0;
                    const std::size_t stepAt =
                        base + static_cast<std::size_t>(step) * tables::kUnlockInstructionStride;
                    std::memcpy(&opcode, table.data() + stepAt, sizeof opcode);
                    std::memcpy(&operand, table.data() + stepAt + 4, sizeof operand);
                    std::array<char, 200> line{};
                    const int told = std::snprintf(
                        line.data(), line.size(),
                        "ev=fullsweep node=%llu field=%zu count=%lld step=%lld opcode=%u operand=%u",
                        static_cast<unsigned long long>(row), field,
                        static_cast<long long>(count), static_cast<long long>(step), opcode,
                        operand);
                    if (told > 0) {
                        core::log::write(core::log::Channel::client, core::log::Level::info,
                                         {line.data(), static_cast<std::size_t>(told)});
                    }
                }
            }
        }

        // TEMPORARY PROBE: three lore books resolve no gate at all, yet the client redacts them,
        // so something gates them that is neither a flag read nor a value read in the two fields
        // checked. Sweep every aligned field of those rows and dump whatever parses as an
        // expression, opcode and operand, rather than guessing which field or opcode it uses.
        if (row == 819 || row == 837 || row == 838) {
            for (std::size_t field = 0; field + 16 <= tables::kNodeRowStride; field += 4) {
                std::int64_t instructions = 0;
                std::int64_t relative = 0;
                std::memcpy(&instructions, table.data() + at + field, sizeof instructions);
                std::memcpy(&relative, table.data() + at + field + 8, sizeof relative);
                if (instructions < 1 || instructions > tables::kNodeExpressionCapacity) {
                    continue;
                }
                const std::size_t pointerAt = at + field + 8;
                const std::int64_t target = static_cast<std::int64_t>(pointerAt) + relative
                                            + static_cast<std::int64_t>(tables::kHeaderSkip);
                if (target < 0
                    || static_cast<std::size_t>(target)
                               + static_cast<std::size_t>(instructions)
                                     * tables::kUnlockInstructionStride
                           > table.size()) {
                    continue;
                }
                const auto base = static_cast<std::size_t>(target);
                for (std::int64_t step = 0; step < instructions; ++step) {
                    std::uint32_t opcode = 0;
                    std::uint32_t operand = 0;
                    const std::size_t stepAt =
                        base + static_cast<std::size_t>(step) * tables::kUnlockInstructionStride;
                    std::memcpy(&opcode, table.data() + stepAt, sizeof opcode);
                    std::memcpy(&operand, table.data() + stepAt + 4, sizeof operand);
                    std::array<char, 180> line{};
                    const int told = std::snprintf(
                        line.data(), line.size(),
                        "ev=noroute node=%llu field=%zu step=%lld opcode=%u operand=%u",
                        static_cast<unsigned long long>(row), field,
                        static_cast<long long>(step), opcode, operand);
                    if (told > 0) {
                        core::log::write(core::log::Channel::client, core::log::Level::info,
                                         {line.data(), static_cast<std::size_t>(told)});
                    }
                }
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
