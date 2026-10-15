#pragma once

#include <cstdint>

#include "mission_script_lua_types.h"
#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

// Peer handles retain identity while their values come from the current published set.
inline constexpr char kPeerCollectionMetatable[] = "sunrise.mission.peers";
inline constexpr char kPeerViewMetatable[] = "sunrise.mission.peer_view";

/** The peer set the host last published. It addresses no row, so it carries a presence marker. */
struct PeerCollectionHandle final {
    std::uint8_t marker{};
};

/** The row is a lookup hint; session identity must still match. */
struct PeerViewHandle final {
    std::uint64_t sessionId{};
    std::uint64_t sessionGeneration{};
    std::uint8_t row{};
};

void push_peers(lua_State* state);
void push_peer_view(lua_State* state, std::uint8_t row);

/** Registers every locked peer userdata shape. */
void register_peer_metatables(lua_State* state);

} // namespace sunrise::server::activity::mission::lua_vm::detail
