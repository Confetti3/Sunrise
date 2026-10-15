#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

#include "mission_api.h"
#include "mission_script_vm_internal.h"

// Table-driven __index. Each handle binds its members in table order, and a compile-time check
// refuses a binding list that differs from the table, so only a declared member is reachable.

namespace sunrise::server::activity::mission::lua_vm::detail {

/** The subject of a receiver whose members read nothing but the Lua state. */
struct NoSubject final {};

/**
 * One member as the VM serves it: a C function for a callable, a pusher for a field.
 * @tparam Subject What the receiver resolved before the key was read.
 */
template <typename Subject> struct Binding final {
    std::string_view name{};
    lua_CFunction method{};
    void (*field)(lua_State*, const Subject&){};
    /** Selects between overloads of one name; null accepts every subject. */
    bool (*accepts)(const Subject&){};
};

/**
 * @return True when the bindings name the table's members in table order, a callable binds a
 * C function, a field binds a pusher, and every overload but the last has an `accepts`.
 */
template <typename Subject>
[[nodiscard]] consteval bool bound_to(std::span<const api::MemberSpec> members,
                                      std::span<const Binding<Subject>> bindings) {
    if (members.size() != bindings.size()) {
        return false;
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        const bool isField = members[index].kind == api::MemberKind::field;
        if (members[index].name != bindings[index].name
            || isField != (bindings[index].field != nullptr)
            || isField == (bindings[index].method != nullptr)) {
            return false;
        }
        for (std::size_t later = index + 1; later < bindings.size(); ++later) {
            if (bindings[later].name == bindings[index].name
                && bindings[index].accepts == nullptr) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Serves one key from a binding list.
 * @param subject What the receiver resolved; a field reads it.
 * @return True when a binding served the key; nothing was pushed otherwise.
 */
template <typename Subject>
[[nodiscard]] bool serve(lua_State* state,
                         std::type_identity_t<std::span<const Binding<Subject>>> bindings,
                         std::string_view key,
                         const Subject& subject) {
    for (const Binding<Subject>& binding : bindings) {
        if (binding.name != key || (binding.accepts != nullptr && !binding.accepts(subject))) {
            continue;
        }
        if (binding.method != nullptr) {
            lua_pushcfunction(state, binding.method);
        } else {
            binding.field(state, subject);
        }
        return true;
    }
    return false;
}

/**
 * Serves one key from a binding list and pushes nil for an undeclared key.
 * @return The Lua result count.
 */
template <typename Subject>
[[nodiscard]] int dispatch(lua_State* state,
                           std::type_identity_t<std::span<const Binding<Subject>>> bindings,
                           std::string_view key,
                           const Subject& subject) {
    if (!serve<Subject>(state, bindings, key, subject)) {
        lua_pushnil(state);
    }
    return 1;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
