// The mission Lua API schema. It is rendered from the same table the mission VM dispatches from,
// so the published file and the runtime cannot disagree.

#include <cstddef>
#include <string>
#include <string_view>

#include "../../../server/activity/mission/mission_api.h"
#include "activity_sdk_lua_artifacts_internal.h"

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal {
namespace {

namespace api = ::sunrise::server::activity::mission::api;

/** @return The schema word of one value kind. */
[[nodiscard]] constexpr std::string_view value_kind_name(api::ValueKind kind) noexcept {
    using Kind = api::ValueKind;
    switch (kind) {
    case Kind::boolean:
        return "boolean";
    case Kind::integer:
        return "integer";
    case Kind::number:
        return "number";
    case Kind::string:
        return "string";
    case Kind::decimalString:
        return "decimal_string";
    case Kind::name:
        return "name";
    case Kind::scalar:
        return "scalar";
    case Kind::selector:
        return "selector";
    case Kind::handle:
        return "handle";
    case Kind::enumValue:
        return "enum";
    case Kind::missionRow:
        return "mission_row";
    case Kind::list:
        return "list";
    case Kind::table:
        return "table";
    case Kind::atom:
        return "atom";
    case Kind::function:
        return "function";
    }
    return "unknown";
}

/** @return The schema word of one member kind. */
[[nodiscard]] constexpr std::string_view member_kind_name(api::MemberKind kind) noexcept {
    using Kind = api::MemberKind;
    switch (kind) {
    case Kind::field:
        return "field";
    case Kind::request:
        return "request";
    case Kind::query:
        return "query";
    case Kind::lookup:
        return "lookup";
    case Kind::update:
        return "update";
    case Kind::maker:
        return "maker";
    }
    return "unknown";
}

/** @return The schema word of one call form. */
[[nodiscard]] constexpr std::string_view call_form_name(api::CallForm form) noexcept {
    using Form = api::CallForm;
    switch (form) {
    case Form::none:
        return "none";
    case Form::named:
        return "named";
    case Form::positional:
        return "positional";
    }
    return "unknown";
}

/** @return The schema word of one argument scope. */
[[nodiscard]] constexpr std::string_view scope_name(api::Scope scope) noexcept {
    using Scope = api::Scope;
    switch (scope) {
    case Scope::none:
        return "none";
    case Scope::receiver:
        return "receiver";
    case Scope::sameRegistry:
        return "same_registry";
    case Scope::sameObject:
        return "same_object";
    case Scope::argument:
        return "argument";
    }
    return "unknown";
}

/** @return The slot properties a gate checks, in schema order. */
[[nodiscard]] Value slot_checks(const api::SlotChecks& checks) {
    Value::Array names{};
    if (checks.componentClass) {
        names.push_back(string("component_class"));
    }
    if (checks.senseSchema) {
        names.push_back(string("sense_schema"));
    }
    if (checks.authSchema) {
        names.push_back(string("auth_schema"));
    }
    if (checks.exactJoin) {
        names.push_back(string("exact_join"));
    }
    if (checks.dialogueCues) {
        names.push_back(string("dialogue_cues"));
    }
    return array(std::move(names));
}

/** @return One slot gate: the slot type and the properties checked against it. */
[[nodiscard]] Value slot_gate(const api::SlotGate& gate) {
    return object({{"slot_type", number(gate.slotType)}, {"checks", slot_checks(gate.checks)}});
}

/**
 * Appends one bound under the names its kind gives it: a count for a list or a table's array
 * part, a byte length for a string, and a value otherwise.
 */
void append_bound(const api::ValueSpec& value, Value::Object& output) {
    const bool counted = value.kind == api::ValueKind::list || value.kind == api::ValueKind::table;
    const bool length = value.kind == api::ValueKind::string || value.kind == api::ValueKind::name
                        || value.kind == api::ValueKind::scalar;
    const std::string_view low = counted ? "min_count" : length ? "min_length" : "minimum";
    const std::string_view high = counted ? "max_count" : length ? "max_length" : "maximum";
    if (value.bound.hasMinimum) {
        output.emplace_back(std::string(low), signed_number(value.bound.minimum));
    }
    if (value.bound.hasMaximum) {
        output.emplace_back(std::string(high), signed_number(value.bound.maximum));
    }
}

/** @return One argument, field, return value or nested value of the table. */
[[nodiscard]] Value value_object(const api::ValueSpec& value) {
    Value::Object output{{"kind", string(value_kind_name(value.kind))},
                         {"optional", boolean(value.optional)}};
    if (!value.name.empty()) {
        output.emplace_back("name", string(value.name));
    }
    if (!value.ref.empty()) {
        output.emplace_back("ref", string(value.ref));
    }
    switch (value.fallback.kind) {
    case api::DefaultKind::none:
        break;
    case api::DefaultKind::boolean:
        output.emplace_back("default", boolean(value.fallback.boolean));
        break;
    case api::DefaultKind::integer:
        output.emplace_back("default", signed_number(value.fallback.integer));
        break;
    case api::DefaultKind::text:
        output.emplace_back("default_text", string(value.fallback.text));
        break;
    }
    append_bound(value, output);
    if (value.slot.slotType != api::kAnySlotType) {
        output.emplace_back("slot_gate", slot_gate(value.slot));
    }
    if (value.scope != api::Scope::none) {
        output.emplace_back("scope", string(scope_name(value.scope)));
    }
    if (value.handleAccepted) {
        output.emplace_back("handle_accepted", boolean(true));
    }
    if (!value.element.empty()) {
        output.emplace_back("element", value_object(value.element.front()));
    }
    if (!value.fields.empty()) {
        Value::Array fields{};
        for (const api::ValueSpec& field : value.fields) {
            fields.push_back(value_object(field));
        }
        output.emplace_back("fields", array(std::move(fields)));
    }
    if (!value.note.empty()) {
        output.emplace_back("note", string(value.note));
    }
    return object(std::move(output));
}

/** @return A list of values in table order. */
[[nodiscard]] Value value_array(std::span<const api::ValueSpec> values) {
    Value::Array output{};
    for (const api::ValueSpec& value : values) {
        output.push_back(value_object(value));
    }
    return array(std::move(output));
}

/**
 * @return One member. A field carries its type; a callable carries its call form, gate,
 * arguments, returns and effect name.
 */
[[nodiscard]] Value
member_object(std::string_view owner, std::string_view ownerKey, const api::MemberSpec& member) {
    Value::Object output{{std::string(ownerKey), string(owner)},
                         {"name", string(member.name)},
                         {"kind", string(member_kind_name(member.kind))}};
    if (member.kind == api::MemberKind::field) {
        output.emplace_back("type", value_object(member.returns.front()));
    } else {
        output.emplace_back("call", string(call_form_name(member.call)));
        output.emplace_back("colon", boolean(member.colon));
        output.emplace_back("arguments", value_array(member.arguments));
        output.emplace_back("returns", value_array(member.returns));
        if (member.call == api::CallForm::named) {
            output.emplace_back("refuses_unknown_keys", boolean(true));
        }
    }
    if (member.gate.slotType != api::kAnySlotType) {
        output.emplace_back("gate", slot_gate(member.gate));
    }
    if (!member.effect.empty()) {
        output.emplace_back("effect", string(member.effect));
    }
    if (!member.note.empty()) {
        output.emplace_back("note", string(member.note));
    }
    return object(std::move(output));
}

/** Appends every receiver and, separately, every member in receiver then table order. */
void append_receivers(Value::Object& output) {
    Value::Array receivers{};
    Value::Array members{};
    for (const api::ReceiverSpec& receiver : api::kReceivers) {
        Value::Object entry{{"name", string(receiver.name)},
                            {"lua_class", string(receiver.luaClass)},
                            {"complete", boolean(receiver.complete)},
                            {"source", string(receiver.source)},
                            {"member_count", number(receiver.members.size())}};
        if (!receiver.entries.empty()) {
            entry.emplace_back("entries", string(receiver.entries));
        }
        if (!receiver.dynamicKeys.empty()) {
            entry.emplace_back("dynamic_keys", string(receiver.dynamicKeys));
        }
        receivers.push_back(object(std::move(entry)));
        for (const api::MemberSpec& member : receiver.members) {
            members.push_back(member_object(receiver.name, "receiver", member));
        }
    }
    output.emplace_back("receivers", array(std::move(receivers)));
    output.emplace_back("members", array(std::move(members)));
}

/** @return Every event kind with its callback and every field its view serves. */
[[nodiscard]] Value events_value() {
    Value::Array events{};
    for (const api::EventSpec& event : api::kEvents) {
        Value::Array groups{};
        Value::Array fields{};
        for (const api::FieldGroup* group : event.groups) {
            groups.push_back(string(group->name));
            for (const api::MemberSpec& member : group->members) {
                fields.push_back(member_object(group->name, "group", member));
            }
        }
        Value::Object entry{{"kind", number(event.kind)},
                            {"name", string(event.screaming)},
                            {"callback", string(event.callback)},
                            {"lua_class", string(event.luaClass)},
                            {"groups", array(std::move(groups))},
                            {"fields", array(std::move(fields))}};
        if (!event.note.empty()) {
            entry.emplace_back("note", string(event.note));
        }
        events.push_back(object(std::move(entry)));
    }
    return array(std::move(events));
}

/** @return The program table keys, the refused keys and the callback parameters. */
[[nodiscard]] Value program_value() {
    Value::Array refused{};
    for (const std::string_view key : api::kRefusedProgramKeys) {
        refused.push_back(string(key));
    }
    return object({{"entries", value_array(api::kProgramEntries)},
                   {"refused", array(std::move(refused))},
                   {"start_parameters", string_array({"context", "state"})},
                   {"event_parameters", string_array({"context", "state", "event"})}});
}

/** @return One closed collection: its entries, or its range and default. */
[[nodiscard]] Value enum_value(const api::EnumSpec& collection) {
    Value::Array entries{};
    // Atom rows index kAtomKinds, so match the entries by identity, not by name.
    const bool atoms = collection.entries.data() == api::kAtomKindEntries.data();
    for (std::size_t index = 0; index < collection.entries.size(); ++index) {
        const api::EnumEntry& entry = collection.entries[index];
        Value::Object row{{"name", string(entry.name)},
                          {std::string(collection.valueName), signed_number(entry.value)}};
        if (!collection.detailName.empty()) {
            row.emplace_back(std::string(collection.detailName), string(entry.detail));
        }
        if (atoms) {
            const api::AtomKindSpec& kind = api::kAtomKinds[index];
            row.emplace_back("fields", value_array(kind.fields));
            row.emplace_back("closed", boolean(kind.closed));
            row.emplace_back("note", string(kind.note));
        }
        entries.push_back(object(std::move(row)));
    }
    Value::Object output{{"name", string(collection.name)},
                         {"lua_class", string(collection.luaClass)},
                         {"entries", array(std::move(entries))},
                         {"note", string(collection.note)}};
    if (collection.range.hasMinimum) {
        output.emplace_back("minimum", signed_number(collection.range.minimum));
        output.emplace_back("maximum", signed_number(collection.range.maximum));
    }
    if (collection.hasDefault) {
        output.emplace_back("default", signed_number(collection.defaultValue));
    }
    return object(std::move(output));
}

/** @return Every schema word a value kind can take, in vocabulary order. */
[[nodiscard]] Value value_kinds_value() {
    Value::Array output{};
    for (std::size_t kind = 0; kind <= static_cast<std::size_t>(api::ValueKind::function); ++kind) {
        output.push_back(string(value_kind_name(static_cast<api::ValueKind>(kind))));
    }
    return array(std::move(output));
}

} // namespace

/** Builds the schema; on failure the output is left empty. */
bool build_mission_api_value(Value& output) noexcept {
    try {
        Value::Object root{{"schema", string("sunrise-mission-lua-api-v1")},
                           {"value_kinds", value_kinds_value()},
                           {"events", events_value()},
                           {"program", program_value()}};
        append_receivers(root);
        Value::Array enums{};
        for (const api::EnumSpec& collection : api::kEnums) {
            enums.push_back(enum_value(collection));
        }
        root.emplace_back("enums", array(std::move(enums)));
        output = object(std::move(root));
        return true;
    } catch (...) {
        output = null_value();
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts::internal
