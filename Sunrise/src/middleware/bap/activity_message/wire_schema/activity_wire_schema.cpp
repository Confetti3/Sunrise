#include "activity_wire_schema.h"

#include <algorithm>
#include <array>

namespace sunrise::middleware::bap::activity_message::wire_schema {
namespace {

/** One authored field documentation grant, addressed by exact message id and field path. */
struct FieldDocGrant final {
    std::uint32_t messageId{};
    std::string_view path{};
    std::string_view meaning{};
    std::string_view source{};
};

#include "activity_wire_field_doc_data.inc"
#include "activity_wire_schema_data.inc"

static_assert(kMessages.size() == kMessageCount);
static_assert(kFields.size() == kFieldCount);

} // namespace

std::span<const MessageDescriptor> messages() noexcept {
    return kMessages;
}

std::span<const FieldDescriptor> all_fields() noexcept {
    return kFields;
}

const MessageDescriptor* find_message(std::uint32_t id) noexcept {
    const std::span<const MessageDescriptor> rows = kMessages;
    return id < rows.size() ? &rows[id] : nullptr;
}

std::span<const FieldDescriptor> fields(const MessageDescriptor& message) noexcept {
    const std::span<const FieldDescriptor> rows = kFields;
    if (message.firstField > rows.size() || message.fieldCount > rows.size() - message.firstField) {
        return {};
    }
    return rows.subspan(message.firstField, message.fieldCount);
}

/** Returns the authored documentation grant for one field, or two empty views. */
FieldDocumentation field_documentation(std::uint32_t messageId, std::string_view path) noexcept {
    const auto found = std::find_if(kFieldDocGrants.begin(),
                                    kFieldDocGrants.end(),
                                    [messageId, path](const FieldDocGrant& grant) noexcept {
                                        return grant.messageId == messageId && grant.path == path;
                                    });
    if (found == kFieldDocGrants.end()) {
        return {};
    }
    return {found->meaning, found->source};
}

/** Returns the layout of one message row. Provenance and coverage fix all but one cell. */
LayoutKind layout_of(std::uint32_t messageId, DefinitionState state, Coverage coverage) noexcept {
    switch (coverage) {
    case Coverage::nameOnly:
    case Coverage::serviceConversion:
        return LayoutKind::absent;
    case Coverage::customWireExact:
        return LayoutKind::packageSelected;
    case Coverage::partialDynamicBody:
        // Graph provenance leaves this cell ambiguous, and three message identities close it.
        if (state == DefinitionState::authored || messageId == 34U || messageId == 39U) {
            return LayoutKind::runtimeSelected;
        }
        return messageId == 40U ? LayoutKind::reflectionWithRawTail : LayoutKind::reflection;
    case Coverage::fixedWireExact:
    case Coverage::variableWire:
        break;
    }
    return state == DefinitionState::authored ? LayoutKind::authored : LayoutKind::reflection;
}

} // namespace sunrise::middleware::bap::activity_message::wire_schema
