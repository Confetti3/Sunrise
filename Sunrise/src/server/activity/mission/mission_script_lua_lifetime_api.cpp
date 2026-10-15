#include <array>
#include <string_view>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_names.h"
#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// The table entry of lifetime:set.
constexpr const api::MemberSpec& kLifetimeSet = api::member(api::Receiver::lifetime, "set");

/** Lifetime members in table order. */
constexpr std::array<Binding<NoSubject>, std::size(api::kLifetimeMembers)> kLifetimeBindings{{
    {"set", &lifetime_set},
}};
static_assert(bound_to<NoSubject>(api::kLifetimeMembers, kLifetimeBindings));

} // namespace

/** Reads the one authorized action on the activity lifetime handle. */
[[nodiscard]] int lifetime_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kLifetimeMetatable));
    return dispatch<NoSubject>(state, kLifetimeBindings, lua_string_view(state, 2), NoSubject{});
}

/** Sets the activity lifetime state the roster reports. Only a named state has a spelling. */
[[nodiscard]] int lifetime_set(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kLifetimeMetatable));
    refuse_unknown_arguments(state, kLifetimeSet);
    const LifetimeStateHandle requested = checked_argument<LifetimeStateHandle>(
        state,
        required_key(kLifetimeSet, "state", api::ValueKind::enumValue),
        kLifetimeStateMetatable);
    CallFrame& frame = active_frame(state);
    Intent intent{};
    intent.kind = IntentKind::setLifetime;
    intent.lifetimeState = requested.state;
    return queue_intent(state, frame, intent);
}

void register_lifetime_metatables(lua_State* state) {
    register_metatable(state, kLifetimeMetatable, &lifetime_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
