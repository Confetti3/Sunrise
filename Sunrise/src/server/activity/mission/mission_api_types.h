#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Vocabulary of the mission Lua API table. The VM and the generated schema both read the table,
// so a member, argument or event field exists only where the table says it does.

namespace sunrise::server::activity::mission::api {

/** What one Lua value is. The generated schema spells each one in snake case. */
enum class ValueKind : std::uint8_t {
    boolean,
    integer,
    number,
    string,
    /** A 64-bit value as decimal digits; a Lua integer cannot hold every one. */
    decimalString,
    /** A timer or variable name: 1 to 63 bytes of letters, digits, `_`, `-`, `.` and `/`. */
    name,
    /** A variable value: boolean, integer, finite number, or string of at most 127 bytes. */
    scalar,
    /** A generated id string or a one-based row number of the `ref` table. */
    selector,
    /** A locked handle of the `ref` receiver. */
    handle,
    /** One entry of the `ref` collection under context.sdk. */
    enumValue,
    /** One row of the `ref` generated mission table. */
    missionRow,
    /** A dense array of `element`, with `bound` as its count. */
    list,
    /** A table with the named `fields`, and an `element` array part when one is declared. */
    table,
    /** One atom table; its shape is the atom kind's entry. */
    atom,
    function,
};

/** What one member does when read or called. */
enum class MemberKind : std::uint8_t {
    /** Read with a dot. */
    field,
    /** Queues one effect and returns its RequestKey. */
    request,
    /** Reads state without changing it. */
    query,
    /** Resolves and returns a handle. */
    lookup,
    /** Changes callback state or a local value; commits with the callback, no RequestKey. */
    update,
    /** Builds a new value. */
    maker,
};

/** How the arguments of one callable member are passed. */
enum class CallForm : std::uint8_t {
    /** A field; it is not called. */
    none,
    /** One table of named arguments. An undeclared key is refused. */
    named,
    /** Plain positional arguments. */
    positional,
};

/** How an argument is tied to the receiver or to another argument. */
enum class Scope : std::uint8_t {
    none,
    /** Must belong to the receiver: this slot, this actor, this squad or this scene. */
    receiver,
    /** Must share the receiver slot's registry. */
    sameRegistry,
    /** Must share the receiver slot's object and registry. */
    sameObject,
    /** Must belong to the slot passed as another argument. */
    argument,
};

/** The kind of the default an omitted argument takes. */
enum class DefaultKind : std::uint8_t {
    none,
    boolean,
    integer,
    /** A described default, for example the authored counts. */
    text,
};

/** The value an omitted optional argument takes. */
struct Default final {
    DefaultKind kind{};
    bool boolean{};
    std::int64_t integer{};
    std::string_view text{};
};

/** Inclusive bounds: of the value, of a string's byte length, or of a list's count. */
struct Bound final {
    bool hasMinimum{};
    bool hasMaximum{};
    std::int64_t minimum{};
    std::int64_t maximum{};
};

/** Slot types are below this value; it means any slot type. */
inline constexpr std::uint32_t kAnySlotType = 0xFFFFFFFFU;

/** Slot properties one gate compares against the authored slot type's expected values. */
struct SlotChecks final {
    bool componentClass{};
    bool senseSchema{};
    bool authSchema{};
    /** The slot carries the exact schema-join flag. */
    bool exactJoin{};
    /** The slot carries the exact dialogue-cue flag. */
    bool dialogueCues{};
};

/** The slot type and properties a slot receiver or slot argument must have. */
struct SlotGate final {
    std::uint32_t slotType{kAnySlotType};
    SlotChecks checks{};
};

/** One argument, field, return value, list element or table field. */
struct ValueSpec final {
    std::string_view name{};
    ValueKind kind{};
    /** Receiver, collection, generated table or atom vocabulary the kind refers to. */
    std::string_view ref{};
    /** An argument may be omitted; a field or return value may be nil. */
    bool optional{};
    Default fallback{};
    Bound bound{};
    SlotGate slot{};
    Scope scope{};
    /** A selector also accepts a handle of the same receiver. */
    bool handleAccepted{};
    std::span<const ValueSpec> element{};
    std::span<const ValueSpec> fields{};
    std::string_view note{};
};

/** One member of a receiver: a field or a callable. */
struct MemberSpec final {
    std::string_view name{};
    MemberKind kind{};
    CallForm call{};
    /** Called with a colon. False for a plain function such as `context.sdk.unit`. */
    bool colon{true};
    SlotGate gate{};
    std::span<const ValueSpec> arguments{};
    std::span<const ValueSpec> returns{};
    /** The `effect` name an effect_result event reports for this request, when it has one. */
    std::string_view effect{};
    std::string_view note{};
};

/** Every receiver a script can hold or index. */
enum class Receiver : std::uint8_t {
    context,
    modConfig,
    state,
    requestKey,
    timers,
    timerRef,
    variables,
    variableRef,
    slot,
    squad,
    countVector,
    scene,
    cohort,
    lifetime,
    peers,
    peer,
    sdk,
    deviceChannels,
    deviceChannel,
    deviceTransitions,
    deviceTransition,
    squadModes,
    squadMode,
    unitScalar,
    worldPosition,
    lifetimeStates,
    lifetimeState,
    boundedLanes,
    boundedLane,
    countList,
    objectRefList,
    actorSequences,
    actorSequence,
    activityMessage,
};

/** One receiver: its schema name, its LuaLS class and the members its __index serves. */
struct ReceiverSpec final {
    Receiver id{};
    std::string_view name{};
    std::string_view luaClass{};
    std::span<const MemberSpec> members{};
    /** Collection whose entry names this receiver also serves, when it has one. */
    std::string_view entries{};
    /** Keys served beyond the members, described; empty when there are none. */
    std::string_view dynamicKeys{};
    /** False when the receiver serves members this table does not list. */
    bool complete{true};
    std::string_view source{};
};

/** One group of event fields that several event kinds share. */
struct FieldGroup final {
    std::string_view name{};
    std::span<const MemberSpec> members{};
};

/** One event kind: its number, its names, and the field groups its view serves. */
struct EventSpec final {
    std::uint8_t kind{};
    std::string_view screaming{};
    std::string_view callback{};
    std::string_view luaClass{};
    std::span<const FieldGroup* const> groups{};
    std::string_view note{};
};

/** One atom kind of an actor program and the fields its table carries. */
struct AtomKindSpec final {
    std::string_view name{};
    std::span<const ValueSpec> fields{};
    /** True when a field outside `fields` is refused; the others ignore unknown fields. */
    bool closed{};
    std::string_view note{};
};

/** One named entry of a context.sdk collection. */
struct EnumEntry final {
    std::string_view name{};
    std::int64_t value{};
    /** A second property, named by the collection's detailName; empty when it has none. */
    std::string_view detail{};
};

/** One context.sdk collection: named entries, or an integer range read with at(n). */
struct EnumSpec final {
    std::string_view name{};
    std::string_view luaClass{};
    std::span<const EnumEntry> entries{};
    std::string_view valueName{"value"};
    std::string_view detailName{};
    Bound range{};
    bool hasDefault{};
    std::int64_t defaultValue{};
    std::string_view note{};
};

// --- Builders ----------------------------------------------------------------------------------
// Short constructors keep each table row on one or two lines.

/** @return A default of one boolean value. */
[[nodiscard]] consteval Default default_boolean(bool value) {
    return {.kind = DefaultKind::boolean, .boolean = value};
}

/** @return A default of one integer value. */
[[nodiscard]] consteval Default default_integer(std::int64_t value) {
    return {.kind = DefaultKind::integer, .integer = value};
}

/** @return A default the schema describes in words. */
[[nodiscard]] consteval Default default_text(std::string_view value) {
    return {.kind = DefaultKind::text, .text = value};
}

/** @return Inclusive bounds on both ends. */
[[nodiscard]] consteval Bound between(std::int64_t minimum, std::int64_t maximum) {
    return {.hasMinimum = true, .hasMaximum = true, .minimum = minimum, .maximum = maximum};
}

/** @return A lower bound only. */
[[nodiscard]] consteval Bound at_least(std::int64_t minimum) {
    return {.hasMinimum = true, .minimum = minimum};
}

/** @return An upper bound only. */
[[nodiscard]] consteval Bound at_most(std::int64_t maximum) {
    return {.hasMaximum = true, .maximum = maximum};
}

// Fixed-width integer ranges the API checks.
inline constexpr std::int64_t kInt32Minimum = -2'147'483'648LL;
inline constexpr std::int64_t kInt32Maximum = 2'147'483'647LL;
inline constexpr std::int64_t kUint32Maximum = 4'294'967'295LL;
inline constexpr std::int64_t kUint16Maximum = 65'535LL;

/** @return The full int32 range. */
[[nodiscard]] consteval Bound int32_range() {
    return between(kInt32Minimum, kInt32Maximum);
}

/** @return The full uint32 range. */
[[nodiscard]] consteval Bound uint32_range() {
    return between(0, kUint32Maximum);
}

/** @return A gate on the slot type alone. */
[[nodiscard]] consteval SlotGate type_gate(std::uint32_t slotType) {
    return {.slotType = slotType};
}

/** @return A gate on the slot type and its Auth schema. */
[[nodiscard]] consteval SlotGate schema_gate(std::uint32_t slotType) {
    return {.slotType = slotType, .checks = {.authSchema = true}};
}

/** @return A gate on the slot type, its Auth schema and the exact flag. */
[[nodiscard]] consteval SlotGate exact_gate(std::uint32_t slotType) {
    return {.slotType = slotType, .checks = {.authSchema = true, .exactJoin = true}};
}

/** @return An exact gate that also checks the component class. */
[[nodiscard]] consteval SlotGate exact_class_gate(std::uint32_t slotType) {
    return {.slotType = slotType,
            .checks = {.componentClass = true, .authSchema = true, .exactJoin = true}};
}

/** @return An exact gate that also checks the component class and the Sense schema. */
[[nodiscard]] consteval SlotGate exact_sensed_gate(std::uint32_t slotType) {
    return {
        .slotType = slotType,
        .checks = {
            .componentClass = true, .senseSchema = true, .authSchema = true, .exactJoin = true}};
}

/** @return One field of the given kind. */
[[nodiscard]] consteval MemberSpec
field(std::string_view name, std::span<const ValueSpec> type, std::string_view note = {}) {
    return {.name = name, .kind = MemberKind::field, .returns = type, .note = note};
}

// --- Lookups the VM and the generator share -----------------------------------------------------

/** @return True when a name is a C string, so the VM can hand it to the Lua C API. */
[[nodiscard]] consteval bool terminated(std::string_view name) {
    return name.empty() || name.data()[name.size()] == '\0';
}

/**
 * Finds one member by name and, for an overloaded slot method, by slot type.
 * Fails compilation when the member is absent.
 */
[[nodiscard]] consteval const MemberSpec& find_member(std::span<const MemberSpec> members,
                                                      std::string_view name,
                                                      std::uint32_t slotType = kAnySlotType) {
    // Do not use continue here; MSVC rejects it in a consteval function.
    const MemberSpec* found = nullptr;
    for (const MemberSpec& member : members) {
        if (member.name == name && (slotType == kAnySlotType || member.gate.slotType == slotType)) {
            if (found != nullptr) {
                throw "overloaded slot member needs its slot type";
            }
            found = &member;
        }
    }
    if (found == nullptr) {
        throw "mission API member is not declared";
    }
    return *found;
}

/** Finds one argument of a member. Fails compilation when the argument is absent. */
[[nodiscard]] consteval const ValueSpec& find_argument(const MemberSpec& member,
                                                       std::string_view name) {
    for (const ValueSpec& argument : member.arguments) {
        if (argument.name == name) {
            return argument;
        }
    }
    throw "mission API argument is not declared";
}

/** Finds one field of a table value. Fails compilation when the field is absent. */
[[nodiscard]] consteval const ValueSpec& find_field(std::span<const ValueSpec> fields,
                                                    std::string_view name) {
    for (const ValueSpec& value : fields) {
        if (value.name == name) {
            return value;
        }
    }
    throw "mission API table field is not declared";
}

/** @return True when no two values share a name and every name is a C string. */
[[nodiscard]] consteval bool distinct_names(std::span<const ValueSpec> values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!terminated(values[index].name)) {
            return false;
        }
        for (std::size_t other = index + 1; other < values.size(); ++other) {
            if (values[index].name == values[other].name) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Checks one value and everything nested in it: only an optional argument has a default, a
 * list has an element, and names are distinct C strings.
 */
[[nodiscard]] consteval bool valid_value(const ValueSpec& value) {
    if ((value.fallback.kind != DefaultKind::none && !value.optional)
        || (value.kind == ValueKind::list && value.element.size() != 1) || value.element.size() > 1
        || !distinct_names(value.fields)) {
        return false;
    }
    for (const ValueSpec& nested : value.element) {
        if (!valid_value(nested)) {
            return false;
        }
    }
    for (const ValueSpec& nested : value.fields) {
        if (!valid_value(nested)) {
            return false;
        }
    }
    return true;
}

/**
 * Checks one member list: names are C strings and unique, except a slot method overloaded by
 * slot type; a field takes no arguments; a callable has a call form.
 */
[[nodiscard]] consteval bool valid_members(std::span<const MemberSpec> members) {
    for (std::size_t index = 0; index < members.size(); ++index) {
        const MemberSpec& member = members[index];
        const bool isField = member.kind == MemberKind::field;
        if (!terminated(member.name) || isField != (member.call == CallForm::none)
            || (isField && (!member.arguments.empty() || member.returns.size() != 1))
            || !distinct_names(member.arguments)) {
            return false;
        }
        for (std::size_t other = index + 1; other < members.size(); ++other) {
            if (members[other].name == member.name
                && members[other].gate.slotType == member.gate.slotType) {
                return false;
            }
        }
        for (const ValueSpec& value : member.arguments) {
            if (!valid_value(value)) {
                return false;
            }
        }
        for (const ValueSpec& value : member.returns) {
            if (!valid_value(value)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace sunrise::server::activity::mission::api
