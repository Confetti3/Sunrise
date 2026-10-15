#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

#include "mission_api.h"
#include "mission_script_vm_internal.h"

// Named effect arguments. Every effect takes one table, so no call site can get an order wrong.
// The declared keys, their kinds and their defaults come from the mission API table.

namespace sunrise::server::activity::mission::lua_vm::detail {

/** Stack index of the one argument table every effect takes. */
inline constexpr int kArgumentTable = 2;

/**
 * Pushes one field of the argument table.
 * A raw read, so a metatable a script installed on its own table reaches nothing.
 * @return The Lua type pushed, LUA_TNIL when no table was passed.
 */
[[nodiscard]] inline int push_argument(lua_State* state, const char* name) {
    if (lua_type(state, kArgumentTable) != LUA_TTABLE) {
        lua_pushnil(state);
        return LUA_TNIL;
    }
    lua_pushstring(state, name);
    return lua_rawget(state, kArgumentTable);
}

/**
 * Refuses an argument table carrying a key the member's table entry does not declare.
 * A missing required key is refused by its own reader, which names it.
 * @param member The table entry of the member being called.
 */
inline void refuse_unknown_arguments(lua_State* state, const api::MemberSpec& member) {
    if (lua_isnoneornil(state, kArgumentTable)) {
        return;
    }
    if (lua_type(state, kArgumentTable) != LUA_TTABLE) {
        static_cast<void>(luaL_error(state, "effect arguments must be one table"));
        return;
    }
    lua_pushnil(state);
    while (lua_next(state, kArgumentTable) != 0) {
        // Reading a non-string key as a string would rewrite it in place and break lua_next.
        if (lua_type(state, -2) != LUA_TSTRING) {
            static_cast<void>(luaL_error(
                state, "%s key: effect argument is not declared", luaL_typename(state, -2)));
            return;
        }
        std::size_t length = 0;
        const char* const text = lua_tolstring(state, -2, &length);
        const std::string_view key{text, length};
        const auto declared =
            std::find_if(member.arguments.begin(),
                         member.arguments.end(),
                         [key](const api::ValueSpec& value) { return value.name == key; });
        if (declared == member.arguments.end()) {
            static_cast<void>(luaL_error(state, "'%s': effect argument is not declared", text));
            return;
        }
        lua_pop(state, 1);
    }
}

/** Errors on a string key `fields` does not declare, or a number key outside 1..#table. */
inline void
refuse_unknown_fields(lua_State* state, int index, std::span<const api::ValueSpec> fields) {
    const int table = lua_absindex(state, index);
    const auto length = static_cast<lua_Integer>(lua_rawlen(state, table));
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            std::size_t keyLength = 0;
            const char* const text = lua_tolstring(state, -2, &keyLength);
            const std::string_view key{text, keyLength};
            if (std::none_of(fields.begin(), fields.end(), [key](const api::ValueSpec& value) {
                    return value.name == key;
                })) {
                static_cast<void>(luaL_error(state, "'%s': table field is not declared", text));
                return;
            }
        } else if (lua_isinteger(state, -2) == 0 || lua_tointeger(state, -2) < 1
                   || lua_tointeger(state, -2) > length) {
            static_cast<void>(luaL_error(
                state, "table key of type %s is not in the array part", luaL_typename(state, -2)));
            return;
        }
        lua_pop(state, 1);
    }
}

// --- Table-checked argument keys ---------------------------------------------------------------
// Each accessor runs at compile time, so a reader that disagrees with the table does not build.

/** One optional boolean argument and the default its table entry declares. */
struct BooleanOption final {
    const char* name{};
    bool fallback{};
};

/** One optional integer argument and the default its table entry declares. */
struct IntegerOption final {
    const char* name{};
    lua_Integer fallback{};
};

/** @return The key of one argument the table declares with this kind and presence. */
[[nodiscard]] consteval const char* argument_key(const api::MemberSpec& member,
                                                 std::string_view name,
                                                 api::ValueKind kind,
                                                 bool optional) {
    const api::ValueSpec& argument = api::find_argument(member, name);
    if (argument.kind != kind || argument.optional != optional) {
        throw "argument reader disagrees with the mission API table";
    }
    return argument.name.data();
}

/** @return The key of one required argument of this kind. */
[[nodiscard]] consteval const char*
required_key(const api::MemberSpec& member, std::string_view name, api::ValueKind kind) {
    return argument_key(member, name, kind, false);
}

/** @return The key of one optional argument of this kind. */
[[nodiscard]] consteval const char*
optional_key(const api::MemberSpec& member, std::string_view name, api::ValueKind kind) {
    return argument_key(member, name, kind, true);
}

/** @return The key of one field of a table argument, declared with this kind and presence. */
[[nodiscard]] consteval const char* table_field_key(const api::MemberSpec& member,
                                                    std::string_view argument,
                                                    std::string_view name,
                                                    api::ValueKind kind,
                                                    bool optional = false) {
    const api::ValueSpec& field =
        api::find_field(api::find_argument(member, argument).fields, name);
    if (field.kind != kind || field.optional != optional) {
        throw "table field reader disagrees with the mission API table";
    }
    return field.name.data();
}

/** @return One optional boolean argument with its declared boolean default. */
[[nodiscard]] consteval BooleanOption boolean_option(const api::MemberSpec& member,
                                                     std::string_view name) {
    const api::ValueSpec& argument = api::find_argument(member, name);
    if (argument.kind != api::ValueKind::boolean || !argument.optional
        || argument.fallback.kind != api::DefaultKind::boolean) {
        throw "boolean reader disagrees with the mission API table";
    }
    return {argument.name.data(), argument.fallback.boolean};
}

/** @return One optional integer argument with its declared integer default. */
[[nodiscard]] consteval IntegerOption integer_option(const api::MemberSpec& member,
                                                     std::string_view name) {
    const api::ValueSpec& argument = api::find_argument(member, name);
    if (argument.kind != api::ValueKind::integer || !argument.optional
        || argument.fallback.kind != api::DefaultKind::integer) {
        throw "integer reader disagrees with the mission API table";
    }
    return {argument.name.data(), argument.fallback.integer};
}

/**
 * Reads one required handle argument by value.
 * @param name Field name in the argument table.
 * @param metatable Metatable the value must carry.
 * @return The handle. The call errors when the field is absent or carries another type.
 */
template <typename Handle>
[[nodiscard]] Handle checked_argument(lua_State* state, const char* name, const char* metatable) {
    static_cast<void>(push_argument(state, name));
    const void* const value = luaL_testudata(state, -1, metatable);
    if (value != nullptr) {
        const Handle handle = *static_cast<const Handle*>(value);
        lua_pop(state, 1);
        return handle;
    }
    luaL_error(state, "%s must be a %s", name, metatable);
    // luaL_error never returns; this satisfies the return contract.
    return Handle{};
}

/**
 * Reads one optional handle argument by value.
 * @param output Written only when the field is present.
 * @return True when the field was present. The call errors when it carries another type.
 */
template <typename Handle>
[[nodiscard]] bool
optional_argument(lua_State* state, const char* name, const char* metatable, Handle& output) {
    if (push_argument(state, name) == LUA_TNIL) {
        lua_pop(state, 1);
        return false;
    }
    const void* const value = luaL_testudata(state, -1, metatable);
    if (value == nullptr) {
        return luaL_error(state, "%s must be a %s", name, metatable) != 0;
    }
    output = *static_cast<const Handle*>(value);
    lua_pop(state, 1);
    return true;
}

/**
 * Reads one optional boolean argument.
 * @param option The key and the default the table declares.
 * @return The field, or the default. The call errors when the field carries another type.
 */
[[nodiscard]] inline bool optional_boolean_argument(lua_State* state, BooleanOption option) {
    if (push_argument(state, option.name) == LUA_TNIL) {
        lua_pop(state, 1);
        return option.fallback;
    }
    if (!lua_isboolean(state, -1)) {
        return luaL_error(state, "%s must be a boolean", option.name) != 0;
    }
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

/**
 * Reads one required string argument and leaves it on the stack.
 * The returned view borrows the Lua string, so the caller must keep that slot until it is done
 * and pop it afterwards.
 * @return The field. The call errors when it is absent or is not a string.
 */
[[nodiscard]] inline std::string_view borrowed_string_argument(lua_State* state, const char* name) {
    static_cast<void>(push_argument(state, name));
    if (lua_type(state, -1) != LUA_TSTRING) {
        static_cast<void>(luaL_error(state, "%s must be a string", name));
        return {};
    }
    std::size_t length = 0;
    const char* const value = lua_tolstring(state, -1, &length);
    return {value, length};
}

/**
 * Reads one required integer argument.
 * @return The field. The call errors when it is absent or is not an integer.
 */
[[nodiscard]] inline lua_Integer checked_integer_argument(lua_State* state, const char* name) {
    static_cast<void>(push_argument(state, name));
    if (!lua_isinteger(state, -1)) {
        return luaL_error(state, "%s must be an integer", name);
    }
    const lua_Integer value = lua_tointeger(state, -1);
    lua_pop(state, 1);
    return value;
}

/**
 * Reads one optional integer argument with a fallback the caller owns.
 * @param fallback Value used when the field is absent.
 */
[[nodiscard]] inline lua_Integer
optional_integer_argument(lua_State* state, const char* name, lua_Integer fallback) {
    if (push_argument(state, name) == LUA_TNIL) {
        lua_pop(state, 1);
        return fallback;
    }
    if (!lua_isinteger(state, -1)) {
        return luaL_error(state, "%s must be an integer", name);
    }
    const lua_Integer value = lua_tointeger(state, -1);
    lua_pop(state, 1);
    return value;
}

/** Reads one optional integer argument with the default its table entry declares. */
[[nodiscard]] inline lua_Integer optional_integer_argument(lua_State* state, IntegerOption option) {
    return optional_integer_argument(state, option.name, option.fallback);
}

/** Reads one required numeric argument without imposing a semantic range. */
[[nodiscard]] inline lua_Number checked_number_argument(lua_State* state, const char* name) {
    static_cast<void>(push_argument(state, name));
    if (!lua_isnumber(state, -1)) {
        return static_cast<lua_Number>(luaL_error(state, "%s must be a number", name));
    }
    const lua_Number value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return value;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
