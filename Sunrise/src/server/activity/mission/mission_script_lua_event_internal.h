#pragma once

#include <string_view>

#include "mission_script_vm_internal.h"

// Event views. One metatable for every kind; the kind picks the field groups, so a read never
// reaches a union arm that kind did not write.

namespace sunrise::server::activity::mission::lua_vm::detail {

/**
 * Reads one event argument of any view.
 * @param index Stack index of the argument.
 * @return The event, after raising a Lua error when the argument is not an event view.
 */
[[nodiscard]] const host::Event& check_event(lua_State* state, int index);

/** @return The catalog Lua name of one event kind. */
[[nodiscard]] std::string_view event_kind_name(host::EventKind kind) noexcept;

} // namespace sunrise::server::activity::mission::lua_vm::detail
