// Mission identity types. Each one is minted from a name or a key the runtime already checked, so
// a script holds the identity instead of spelling it.

#include <array>
#include <cstdint>
#include <string_view>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** Compares two keys the runtime minted. Concatenating a decimal string reaches nothing. */
[[nodiscard]] int request_key_matches(lua_State* state) {
    const auto* const handle =
        static_cast<const RequestKeyHandle*>(luaL_checkudata(state, 1, kRequestKeyMetatable));
    const auto* const other =
        static_cast<const RequestKeyHandle*>(luaL_checkudata(state, 2, kRequestKeyMetatable));
    lua_pushboolean(state, handle->key == other->key ? 1 : 0);
    return 1;
}

/** Request key members in table order. */
constexpr std::array<Binding<RequestKeyHandle>, std::size(api::kRequestKeyMembers)>
    kRequestKeyBindings{{
        {"matches", &request_key_matches},
        {"value",
         nullptr,
         [](lua_State* state, const RequestKeyHandle& handle) {
             push_u64_string(state, handle.key);
         }},
    }};
static_assert(bound_to<RequestKeyHandle>(api::kRequestKeyMembers, kRequestKeyBindings));

/** Lua index for one request key: matches and value. */
[[nodiscard]] int request_key_index(lua_State* state) {
    const auto* const handle =
        static_cast<const RequestKeyHandle*>(luaL_checkudata(state, 1, kRequestKeyMetatable));
    return dispatch<RequestKeyHandle>(
        state, kRequestKeyBindings, lua_string_view(state, 2), *handle);
}

/** Pushes the stored name of one checked timer or variable name. */
template <typename Handle> void push_ref_name(lua_State* state, const Handle& handle) {
    const std::string_view name = state_key_view(handle.key);
    lua_pushlstring(state, name.data(), name.size());
}

/** Timer name members in table order. */
constexpr std::array<Binding<TimerRefHandle>, std::size(api::kNameRefMembers)> kTimerRefBindings{{
    {"name", nullptr, &push_ref_name<TimerRefHandle>},
}};
static_assert(bound_to<TimerRefHandle>(api::kNameRefMembers, kTimerRefBindings));

/** Variable name members in table order. */
constexpr std::array<Binding<VariableRefHandle>, std::size(api::kNameRefMembers)>
    kVariableRefBindings{{
        {"name", nullptr, &push_ref_name<VariableRefHandle>},
    }};
static_assert(bound_to<VariableRefHandle>(api::kNameRefMembers, kVariableRefBindings));

/** Lua index for one timer reference: its name. */
[[nodiscard]] int timer_ref_index(lua_State* state) {
    const auto* const handle =
        static_cast<const TimerRefHandle*>(luaL_checkudata(state, 1, kTimerRefMetatable));
    return dispatch<TimerRefHandle>(state, kTimerRefBindings, lua_string_view(state, 2), *handle);
}

[[nodiscard]] int timer_ref_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kTimerRefCollectionMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission timer name is invalid");
    }
    push_timer_ref(state, key);
    return 1;
}

/** context.timers members in table order. */
constexpr std::array<Binding<NoSubject>, std::size(api::kTimersMembers)> kTimersBindings{{
    {"resolve", &timer_ref_collection_resolve},
    {"capacity",
     nullptr,
     [](lua_State* state, const NoSubject&) {
         lua_pushinteger(state, static_cast<lua_Integer>(kTimerCapacity));
     }},
}};
static_assert(bound_to<NoSubject>(api::kTimersMembers, kTimersBindings));

/** Lua index for the timer reference collection: `resolve` and `capacity`, else nil. */
[[nodiscard]] int timer_ref_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kTimerRefCollectionMetatable));
    return dispatch<NoSubject>(state, kTimersBindings, lua_string_view(state, 2), NoSubject{});
}

/** Lua index for one variable reference: its name. */
[[nodiscard]] int variable_ref_index(lua_State* state) {
    const auto* const handle =
        static_cast<const VariableRefHandle*>(luaL_checkudata(state, 1, kVariableRefMetatable));
    return dispatch<VariableRefHandle>(
        state, kVariableRefBindings, lua_string_view(state, 2), *handle);
}

[[nodiscard]] int variable_ref_collection_resolve(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kVariableRefCollectionMetatable));
    StateKey key{};
    if (!parse_state_key(state, 2, key)) {
        return luaL_argerror(state, 2, "mission variable name is invalid");
    }
    push_variable_ref(state, key);
    return 1;
}

/** context.variables members in table order. */
constexpr std::array<Binding<NoSubject>, std::size(api::kVariablesMembers)> kVariablesBindings{{
    {"resolve", &variable_ref_collection_resolve},
    {"capacity",
     nullptr,
     [](lua_State* state, const NoSubject&) {
         lua_pushinteger(state, static_cast<lua_Integer>(kVariableCapacity));
     }},
}};
static_assert(bound_to<NoSubject>(api::kVariablesMembers, kVariablesBindings));

/** Lua index for the variable reference collection: `resolve` and `capacity`, else nil. */
[[nodiscard]] int variable_ref_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kVariableRefCollectionMetatable));
    return dispatch<NoSubject>(state, kVariablesBindings, lua_string_view(state, 2), NoSubject{});
}

} // namespace

void push_request_key(lua_State* state, std::uint64_t key) {
    push_handle(state, kRequestKeyMetatable, RequestKeyHandle{key});
}

void push_timer_ref(lua_State* state, const StateKey& key) {
    push_handle(state, kTimerRefMetatable, TimerRefHandle{key});
}

void push_variable_ref(lua_State* state, const StateKey& key) {
    push_handle(state, kVariableRefMetatable, VariableRefHandle{key});
}

void register_key_metatables(lua_State* state) {
    register_metatable(state, kRequestKeyMetatable, &request_key_index);
    register_metatable(state, kTimerRefMetatable, &timer_ref_index);
    register_metatable(state, kTimerRefCollectionMetatable, &timer_ref_collection_index);
    register_metatable(state, kVariableRefMetatable, &variable_ref_index);
    register_metatable(state, kVariableRefCollectionMetatable, &variable_ref_collection_index);
}

void push_timer_ref_collection(lua_State* state) {
    push_handle(state, kTimerRefCollectionMetatable, TimerRefCollectionHandle{});
}

void push_variable_ref_collection(lua_State* state) {
    push_handle(state, kVariableRefCollectionMetatable, VariableRefCollectionHandle{});
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
