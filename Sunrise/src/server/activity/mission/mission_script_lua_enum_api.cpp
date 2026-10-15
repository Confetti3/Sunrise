// Closed vocabularies. Each one is a set of named values inside a wider wire field, so the
// encodings the client cannot survive have no spelling a script can write.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../state/activity/membership/definition.h"
#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

// Event views index per-channel arrays by these values.
static_assert([] {
    for (const api::EnumEntry& entry : api::kDeviceChannelEntries) {
        if (entry.value < 0
            || static_cast<std::size_t>(entry.value) >= scriptable_auth::kType23ChannelCount) {
            return false;
        }
    }
    return std::size(api::kDeviceChannelEntries) == scriptable_auth::kType23ChannelCount;
}());

/** Device channel members in table order. */
constexpr std::array<Binding<DeviceChannelHandle>, std::size(api::kDeviceChannelMembers)>
    kDeviceChannelBindings{{
        {"name",
         nullptr,
         [](lua_State* state, const DeviceChannelHandle& handle) {
             const std::string_view name = device_channel_name(handle.channel);
             lua_pushlstring(state, name.data(), name.size());
         }},
        {"value",
         nullptr,
         [](lua_State* state, const DeviceChannelHandle& handle) {
             lua_pushinteger(state, handle.channel);
         }},
    }};
static_assert(bound_to<DeviceChannelHandle>(api::kDeviceChannelMembers, kDeviceChannelBindings));

/** Lua index for one device channel: its name and wire value. */
[[nodiscard]] int device_channel_index(lua_State* state) {
    const auto* const handle =
        static_cast<const DeviceChannelHandle*>(luaL_checkudata(state, 1, kDeviceChannelMetatable));
    return dispatch<DeviceChannelHandle>(
        state, kDeviceChannelBindings, lua_string_view(state, 2), *handle);
}

/** Device channel collection members in table order; channel names are served after them. */
constexpr std::array<Binding<NoSubject>, std::size(api::kDeviceChannelsMembers)>
    kDeviceChannelsBindings{{
        {"count",
         nullptr,
         [](lua_State* state, const NoSubject&) {
             lua_pushinteger(state,
                             static_cast<lua_Integer>(std::size(api::kDeviceChannelEntries)));
         }},
    }};
static_assert(bound_to<NoSubject>(api::kDeviceChannelsMembers, kDeviceChannelsBindings));

/** Lua index for the device channel collection: `count` or a channel name, else nil. */
[[nodiscard]] int device_channel_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kDeviceChannelCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    std::uint8_t channel = 0;
    if (serve<NoSubject>(state, kDeviceChannelsBindings, key, NoSubject{})) {
        return 1;
    }
    if (device_channel_value(key, channel)) {
        push_device_channel(state, channel);
    } else {
        lua_pushnil(state);
    }
    return 1;
}

/** Device transition members in table order. */
constexpr std::array<Binding<DeviceTransition>, std::size(api::kDeviceTransitionMembers)>
    kDeviceTransitionBindings{{
        {"name",
         nullptr,
         [](lua_State* state, const DeviceTransition& row) {
             lua_pushlstring(state, row.name.data(), row.name.size());
         }},
        {"channel",
         nullptr,
         [](lua_State* state, const DeviceTransition& row) {
             push_device_channel(state, static_cast<std::uint8_t>(row.channel));
         }},
        {"value",
         nullptr,
         [](lua_State* state, const DeviceTransition& row) { push_unit_scalar(state, row.value); }},
    }};
static_assert(bound_to<DeviceTransition>(api::kDeviceTransitionMembers, kDeviceTransitionBindings));

/** Lua index for one device transition: its name and wire value. */
[[nodiscard]] int device_transition_index(lua_State* state) {
    const auto* const handle = static_cast<const DeviceTransitionHandle*>(
        luaL_checkudata(state, 1, kDeviceTransitionMetatable));
    if (handle->row >= kDeviceTransitions.size()) {
        return luaL_error(state, "device transition row is outside the vocabulary");
    }
    return dispatch<DeviceTransition>(state,
                                      kDeviceTransitionBindings,
                                      lua_string_view(state, 2),
                                      kDeviceTransitions[handle->row]);
}

[[nodiscard]] int device_transition_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kDeviceTransitionCollectionMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || static_cast<std::size_t>(row) > kDeviceTransitions.size()) {
        return luaL_error(state, "device transition row is outside the vocabulary");
    }
    push_device_transition(state, static_cast<std::uint8_t>(row - 1));
    return 1;
}

/** Device transition collection members in table order; transition names follow them. */
constexpr std::array<Binding<NoSubject>, std::size(api::kDeviceTransitionsMembers)>
    kDeviceTransitionsBindings{{
        {"count",
         nullptr,
         [](lua_State* state, const NoSubject&) {
             lua_pushinteger(state, static_cast<lua_Integer>(kDeviceTransitions.size()));
         }},
        {"at", &device_transition_collection_at},
    }};
static_assert(bound_to<NoSubject>(api::kDeviceTransitionsMembers, kDeviceTransitionsBindings));

/** Lua index for the device transition collection: `count`, `at` or a transition name. */
[[nodiscard]] int device_transition_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kDeviceTransitionCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (serve<NoSubject>(state, kDeviceTransitionsBindings, key, NoSubject{})) {
        return 1;
    }
    for (std::size_t row = 0; row < kDeviceTransitions.size(); ++row) {
        if (kDeviceTransitions[row].name == key) {
            push_device_transition(state, static_cast<std::uint8_t>(row));
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

/** Lifetime state members in table order. */
constexpr std::array<Binding<LifetimeStateHandle>, std::size(api::kLifetimeStateMembers)>
    kLifetimeStateBindings{{
        {"value",
         nullptr,
         [](lua_State* state, const LifetimeStateHandle& handle) {
             lua_pushinteger(state, handle.state);
         }},
    }};
static_assert(bound_to<LifetimeStateHandle>(api::kLifetimeStateMembers, kLifetimeStateBindings));

/** Lua index for one lifetime state: `value`, else nil. */
[[nodiscard]] int lifetime_state_index(lua_State* state) {
    const auto* const handle =
        static_cast<const LifetimeStateHandle*>(luaL_checkudata(state, 1, kLifetimeStateMetatable));
    return dispatch<LifetimeStateHandle>(
        state, kLifetimeStateBindings, lua_string_view(state, 2), *handle);
}

/** Selects one state inside the client jump table. There is no expression that reaches 11. */
[[nodiscard]] int lifetime_state_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kLifetimeStateCollectionMetatable));
    const lua_Integer requested = luaL_checkinteger(state, 2);
    if (requested < 0 || requested > host::kMaximumLifetimeState) {
        return luaL_error(state, "activity lifetime state is outside the client jump table");
    }
    push_lifetime_state(state, static_cast<std::uint8_t>(requested));
    return 1;
}

/** Lifetime state collection members in table order. */
constexpr std::array<Binding<NoSubject>, std::size(api::kLifetimeStatesMembers)>
    kLifetimeStatesBindings{{
        {"count",
         nullptr,
         [](lua_State* state, const NoSubject&) { lua_pushinteger(state, kLifetimeStateCount); }},
        {"at", &lifetime_state_collection_at},
        {"default",
         nullptr,
         [](lua_State* state, const NoSubject&) {
             push_lifetime_state(state, host::kDefaultLifetimeState);
         }},
    }};
static_assert(bound_to<NoSubject>(api::kLifetimeStatesMembers, kLifetimeStatesBindings));

/** Lua index for the lifetime state collection: `count`, `at` and `default`, else nil. */
[[nodiscard]] int lifetime_state_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kLifetimeStateCollectionMetatable));
    return dispatch<NoSubject>(
        state, kLifetimeStatesBindings, lua_string_view(state, 2), NoSubject{});
}

} // namespace

/** @return The script-facing name of one device channel, or "unknown". */
std::string_view device_channel_name(std::uint8_t channel) noexcept {
    for (const api::EnumEntry& entry : api::kDeviceChannelEntries) {
        if (entry.value == channel) {
            return entry.name;
        }
    }
    return "unknown";
}

/** Maps a device channel name to its wire value. @return False when the name is unknown. */
bool device_channel_value(std::string_view name, std::uint8_t& output) noexcept {
    for (const api::EnumEntry& entry : api::kDeviceChannelEntries) {
        if (entry.name == name) {
            output = static_cast<std::uint8_t>(entry.value);
            return true;
        }
    }
    return false;
}

void push_device_channel(lua_State* state, std::uint8_t channel) {
    push_handle(state, kDeviceChannelMetatable, DeviceChannelHandle{channel});
}

void push_device_transition(lua_State* state, std::uint8_t row) {
    push_handle(state, kDeviceTransitionMetatable, DeviceTransitionHandle{row});
}

void push_lifetime_state(lua_State* state, std::uint8_t value) {
    push_handle(state, kLifetimeStateMetatable, LifetimeStateHandle{value});
}

void register_enum_metatables(lua_State* state) {
    register_metatable(state, kDeviceChannelMetatable, &device_channel_index);
    register_metatable(state, kDeviceChannelCollectionMetatable, &device_channel_collection_index);
    register_metatable(state, kDeviceTransitionMetatable, &device_transition_index);
    register_metatable(
        state, kDeviceTransitionCollectionMetatable, &device_transition_collection_index);
    register_metatable(state, kLifetimeStateMetatable, &lifetime_state_index);
    register_metatable(state, kLifetimeStateCollectionMetatable, &lifetime_state_collection_index);
}

/** Serves the activity table's enum collections. @return False for any other key. */
bool push_enum_activity_member(lua_State* state, std::string_view key) {
    if (key == "client_teleport_reset") {
        lua_pushinteger(state, state::activity::membership::kClientTeleportResetState);
        return true;
    }
    if (key == "device_channels") {
        push_handle(state, kDeviceChannelCollectionMetatable, DeviceChannelCollectionHandle{});
        return true;
    }
    if (key == "device_transitions") {
        push_handle(
            state, kDeviceTransitionCollectionMetatable, DeviceTransitionCollectionHandle{});
        return true;
    }
    if (key == "lifetime_states") {
        push_handle(state, kLifetimeStateCollectionMetatable, LifetimeStateCollectionHandle{});
        return true;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
