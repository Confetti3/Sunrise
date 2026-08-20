#include "package_socket_plug_build.h"

#include <array>
#include <cstring>
#include <limits>

#include "../../../../middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Native postfix opcodes for value, literal, and greater-than-or-equal. */
constexpr std::uint32_t kValueOpcode = 10;
constexpr std::uint32_t kLiteralOpcode = 11;
constexpr std::uint32_t kGreaterEqualOpcode = 14;
/** Each postfix token is an opcode and one 32-bit operand. */
constexpr std::size_t kExpressionTokenSize = 8;
constexpr std::size_t kCompletionExpressionTokenCount = 3;

} // namespace

void read_catalyst_completion_condition(
    std::span<const std::byte> definition,
    std::uint16_t itemDefinitionIndex,
    catalysts::CompletionCondition& output) noexcept {
    output = {};
    output.itemDefinitionIndex = itemDefinitionIndex;
    for (std::size_t descriptor = 0;
         descriptor + 2 * sizeof(std::uint64_t) <= definition.size();
         descriptor += sizeof(std::uint64_t)) {
        tables::Array expression{};
        if (!tables::find_array_at(definition, descriptor, expression)
            || expression.count != kCompletionExpressionTokenCount
            || expression.elementClass != tables::kInvestmentExpressionRowClass
            || expression.dataOffset > definition.size()
            || definition.size() - expression.dataOffset
                   < kCompletionExpressionTokenCount * kExpressionTokenSize) {
            continue;
        }
        std::array<std::uint32_t, kCompletionExpressionTokenCount * 2> tokens{};
        std::memcpy(tokens.data(),
                    definition.data() + expression.dataOffset,
                    tokens.size() * sizeof(tokens.front()));
        if (tokens[0] != kValueOpcode || tokens[2] != kLiteralOpcode
            || tokens[4] != kGreaterEqualOpcode || tokens[5] != UINT32_MAX
            || tokens[1] >= catalysts::kUnavailableCompletionValueIndex
            || tokens[3] == 0
            || tokens[3]
                   > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            continue;
        }
        const auto valueIndex = static_cast<std::uint16_t>(tokens[1]);
        const auto value = static_cast<std::int32_t>(tokens[3]);
        if (output.state == catalysts::CompletionConditionState::absent) {
            output.valueIndex = valueIndex;
            output.value = value;
            output.state = catalysts::CompletionConditionState::present;
        } else if (output.valueIndex != valueIndex || output.value != value) {
            output.valueIndex = catalysts::kUnavailableCompletionValueIndex;
            output.value = 0;
            output.state = catalysts::CompletionConditionState::ambiguous;
            return;
        }
    }
}

} // namespace sunrise::client::content::items::packages
