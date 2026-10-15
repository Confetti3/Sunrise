// Bounded lists for the count and object-reference lanes that walk past a fixed client array.
// A list cannot grow past its lane's array, so the count it produces is always in range.
// TODO: the typed Auth draft consumes these; until it lands they have no caller.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

/** @return The lane a list handle names, or null when the row is outside the table. */
[[nodiscard]] const BoundedLane* bounded_lane(std::uint8_t row) noexcept {
    return row < kBoundedLanes.size() ? &kBoundedLanes[row] : nullptr;
}

[[nodiscard]] int count_list_at(lua_State* state) {
    const auto* const handle =
        static_cast<const CountListHandle*>(luaL_checkudata(state, 1, kCountListMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || row > handle->count) {
        return luaL_error(state, "count list row is outside the list");
    }
    lua_pushinteger(state, handle->values[static_cast<std::size_t>(row - 1)]);
    return 1;
}

/** Appends one element. The lane's array size is the ceiling, so the count cannot overrun. */
[[nodiscard]] int count_list_append(lua_State* state) {
    auto* const handle =
        static_cast<CountListHandle*>(luaL_checkudata(state, 1, kCountListMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "count list lane is unknown");
    }
    if (handle->count >= lane->capacity) {
        return luaL_error(state, "count list is full");
    }
    const lua_Integer value = luaL_checkinteger(state, 2);
    if (value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return luaL_error(state, "count list value is outside int32");
    }
    handle->values[handle->count] = static_cast<std::int32_t>(value);
    ++handle->count;
    return 0;
}

/** A list's length and the lane it belongs to; every list field reads these two. */
struct ListView final {
    std::uint8_t count{};
    const BoundedLane* lane{};
};

/** Pushes the length of a list. */
void push_list_count(lua_State* state, const ListView& list) {
    lua_pushinteger(state, list.count);
}

/** Pushes the capacity of a list's lane. */
void push_list_capacity(lua_State* state, const ListView& list) {
    lua_pushinteger(state, list.lane->capacity);
}

/** Pushes the name of a list's lane. */
void push_list_lane(lua_State* state, const ListView& list) {
    lua_pushlstring(state, list.lane->name.data(), list.lane->name.size());
}

/** Count list members in table order. */
constexpr std::array<Binding<ListView>, std::size(api::kCountListMembers)> kCountListBindings{{
    {"count", nullptr, &push_list_count},
    {"capacity", nullptr, &push_list_capacity},
    {"lane", nullptr, &push_list_lane},
    {"at", &count_list_at},
    {"append", &count_list_append},
}};
static_assert(bound_to<ListView>(api::kCountListMembers, kCountListBindings));

/** Lua index for a count list: the lane's counts and bounds. Errors on an unknown lane. */
[[nodiscard]] int count_list_index(lua_State* state) {
    const auto* const handle =
        static_cast<const CountListHandle*>(luaL_checkudata(state, 1, kCountListMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "count list lane is unknown");
    }
    return dispatch<ListView>(
        state, kCountListBindings, lua_string_view(state, 2), ListView{handle->count, lane});
}

/** Pushes the slot at one 1-based list row. Raises a Lua error when the row is out of range. */
[[nodiscard]] int object_ref_list_at(lua_State* state) {
    const auto* const handle =
        static_cast<const ObjectRefListHandle*>(luaL_checkudata(state, 1, kObjectRefListMetatable));
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || row > handle->count) {
        return luaL_error(state, "object reference row is outside the list");
    }
    push_handle(
        state, kSlotMetatable, SlotHandle{handle->slotRows[static_cast<std::size_t>(row - 1)]});
    return 1;
}

/** Appends one installed object. The element is a resolved slot, never an index a script wrote. */
[[nodiscard]] int object_ref_list_append(lua_State* state) {
    auto* const handle =
        static_cast<ObjectRefListHandle*>(luaL_checkudata(state, 1, kObjectRefListMetatable));
    const auto* const slotHandle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 2, kSlotMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "object reference lane is unknown");
    }
    if (handle->count >= lane->capacity) {
        return luaL_error(state, "object reference list is full");
    }
    SlotDefinition slot{};
    if (!current_slot(state, *slotHandle, slot)) {
        return luaL_error(state, "activity slot is stale");
    }
    handle->slotRows[handle->count] = slotHandle->localRow;
    ++handle->count;
    return 0;
}

/** Object reference list members in table order. */
constexpr std::array<Binding<ListView>, std::size(api::kObjectRefListMembers)>
    kObjectRefListBindings{{
        {"count", nullptr, &push_list_count},
        {"capacity", nullptr, &push_list_capacity},
        {"lane", nullptr, &push_list_lane},
        {"at", &object_ref_list_at},
        {"append", &object_ref_list_append},
    }};
static_assert(bound_to<ListView>(api::kObjectRefListMembers, kObjectRefListBindings));

/** Lua index for an object reference list: the lane's refs and bounds. */
[[nodiscard]] int object_ref_list_index(lua_State* state) {
    const auto* const handle =
        static_cast<const ObjectRefListHandle*>(luaL_checkudata(state, 1, kObjectRefListMetatable));
    const BoundedLane* const lane = bounded_lane(handle->lane);
    if (lane == nullptr) {
        return luaL_error(state, "object reference lane is unknown");
    }
    return dispatch<ListView>(
        state, kObjectRefListBindings, lua_string_view(state, 2), ListView{handle->count, lane});
}

/** Mints the empty list of the lane at stack index one. */
[[nodiscard]] int bounded_lane_list(lua_State* state) {
    const auto* const handle =
        static_cast<const BoundedLaneHandle*>(luaL_checkudata(state, 1, kBoundedLaneMetatable));
    const BoundedLane* const lane = bounded_lane(handle->row);
    if (lane == nullptr) {
        return luaL_error(state, "bounded lane is unknown");
    }
    if (lane->kind == BoundedLaneKind::count) {
        CountListHandle list{};
        list.lane = handle->row;
        push_handle(state, kCountListMetatable, list);
    } else {
        ObjectRefListHandle list{};
        list.lane = handle->row;
        push_handle(state, kObjectRefListMetatable, list);
    }
    return 1;
}

/** Bounded lane members in table order. */
constexpr std::array<Binding<BoundedLane>, std::size(api::kBoundedLaneMembers)>
    kBoundedLaneBindings{{
        {"name",
         nullptr,
         [](lua_State* state, const BoundedLane& lane) {
             lua_pushlstring(state, lane.name.data(), lane.name.size());
         }},
        {"capacity",
         nullptr,
         [](lua_State* state, const BoundedLane& lane) { lua_pushinteger(state, lane.capacity); }},
        {"kind",
         nullptr,
         [](lua_State* state, const BoundedLane& lane) {
             const std::string_view kind = api::bounded_lane_kind_name(lane.kind);
             lua_pushlstring(state, kind.data(), kind.size());
         }},
        {"list", &bounded_lane_list},
    }};
static_assert(bound_to<BoundedLane>(api::kBoundedLaneMembers, kBoundedLaneBindings));

/** Lua index for one bounded lane: its name, capacity, kind and list maker. */
[[nodiscard]] int bounded_lane_index(lua_State* state) {
    const auto* const handle =
        static_cast<const BoundedLaneHandle*>(luaL_checkudata(state, 1, kBoundedLaneMetatable));
    const BoundedLane* const lane = bounded_lane(handle->row);
    if (lane == nullptr) {
        return luaL_error(state, "bounded lane is unknown");
    }
    return dispatch<BoundedLane>(state, kBoundedLaneBindings, lua_string_view(state, 2), *lane);
}

/** Bounded lane collection members in table order; lane names are served after them. */
constexpr std::array<Binding<NoSubject>, std::size(api::kBoundedLanesMembers)>
    kBoundedLanesBindings{{
        {"count",
         nullptr,
         [](lua_State* state, const NoSubject&) {
             lua_pushinteger(state, static_cast<lua_Integer>(kBoundedLanes.size()));
         }},
    }};
static_assert(bound_to<NoSubject>(api::kBoundedLanesMembers, kBoundedLanesBindings));

/** Lua index for the bounded lane collection: `count` or a lane name, else nil. */
[[nodiscard]] int bounded_lane_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kBoundedLaneCollectionMetatable));
    const std::string_view key = lua_string_view(state, 2);
    if (serve<NoSubject>(state, kBoundedLanesBindings, key, NoSubject{})) {
        return 1;
    }
    for (std::size_t row = 0; row < kBoundedLanes.size(); ++row) {
        if (kBoundedLanes[row].name == key) {
            push_bounded_lane(state, static_cast<std::uint8_t>(row));
            return 1;
        }
    }
    lua_pushnil(state);
    return 1;
}

/** World position members in table order: one lane each. */
constexpr std::array<Binding<WorldPositionHandle>, std::size(api::kWorldPositionMembers)>
    kWorldPositionBindings{{
        {"x",
         nullptr,
         [](lua_State* state, const WorldPositionHandle& handle) {
             lua_pushnumber(state, handle.value[0]);
         }},
        {"y",
         nullptr,
         [](lua_State* state, const WorldPositionHandle& handle) {
             lua_pushnumber(state, handle.value[1]);
         }},
        {"z",
         nullptr,
         [](lua_State* state, const WorldPositionHandle& handle) {
             lua_pushnumber(state, handle.value[2]);
         }},
    }};
static_assert(bound_to<WorldPositionHandle>(api::kWorldPositionMembers, kWorldPositionBindings));

/** Lua index for a world position: `x`, `y` and `z` select a lane, any other key is nil. */
[[nodiscard]] int world_position_index(lua_State* state) {
    const auto* const handle =
        static_cast<const WorldPositionHandle*>(luaL_checkudata(state, 1, kWorldPositionMetatable));
    return dispatch<WorldPositionHandle>(
        state, kWorldPositionBindings, lua_string_view(state, 2), *handle);
}

/** Mints one finite world position. The client applies no magnitude bound, so none is here. */
[[nodiscard]] int world_position_new(lua_State* state) {
    std::array<float, 3> value{};
    for (int lane = 0; lane < 3; ++lane) {
        if (lua_type(state, lane + 1) != LUA_TNUMBER) {
            return luaL_error(state, "world position takes three finite numbers");
        }
        const lua_Number requested = lua_tonumber(state, lane + 1);
        value[static_cast<std::size_t>(lane)] = static_cast<float>(requested);
        if (!std::isfinite(requested) || !std::isfinite(value[static_cast<std::size_t>(lane)])) {
            return luaL_error(state, "world position takes three finite numbers");
        }
    }
    push_world_position(state, value);
    return 1;
}

} // namespace

void push_bounded_lane(lua_State* state, std::uint8_t row) {
    push_handle(state, kBoundedLaneMetatable, BoundedLaneHandle{row});
}

void push_world_position(lua_State* state, const std::array<float, 3>& value) {
    push_handle(state, kWorldPositionMetatable, WorldPositionHandle{value});
}

void register_bounded_metatables(lua_State* state) {
    register_metatable(state, kBoundedLaneCollectionMetatable, &bounded_lane_collection_index);
    register_metatable(state, kBoundedLaneMetatable, &bounded_lane_index);
    register_metatable(state, kCountListMetatable, &count_list_index);
    register_metatable(state, kObjectRefListMetatable, &object_ref_list_index);
    register_metatable(state, kWorldPositionMetatable, &world_position_index);
}

/** Pushes one bounded-lane ActivityView member. @return False when the key is not ours. */
bool push_bounded_activity_member(lua_State* state, std::string_view key) {
    if (key == "bounded_lanes") {
        push_handle(state, kBoundedLaneCollectionMetatable, BoundedLaneCollectionHandle{});
        return true;
    }
    if (key == "position") {
        lua_pushcfunction(state, &world_position_new);
        return true;
    }
    return false;
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
