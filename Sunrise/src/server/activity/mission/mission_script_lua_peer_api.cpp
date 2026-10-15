// Peer reads. context.peers mints a view over the peer set the host last published.
// Each read finds the peer by session id and generation, so a held view never answers for
// another peer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mission_script_lua_dispatch.h"
#include "mission_script_lua_internal.h"
#include "mission_script_lua_peer_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

namespace {

/** A retained handle can follow its peer across reordering, but cannot name its replacement. */
[[nodiscard]] const PeerSession& current_peer(lua_State* state, const PeerViewHandle& handle) {
    const Impl* const impl = impl_from_state(state);
    const auto matches = [&handle](const PeerSession& peer) {
        return peer.sessionId == handle.sessionId
               && peer.sessionGeneration == handle.sessionGeneration;
    };
    if (handle.row < impl->peerCount && matches(impl->peers[handle.row])) {
        return impl->peers[handle.row];
    }
    for (std::size_t row = 0; row < impl->peerCount; ++row) {
        if (matches(impl->peers[row])) {
            return impl->peers[row];
        }
    }
    raise_lua_error(state, "peer session is no longer in the published set");
}

/** Mints the view of one peer the published set still holds. */
[[nodiscard]] int peer_collection_at(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kPeerCollectionMetatable));
    const Impl* const impl = impl_from_state(state);
    const lua_Integer row = luaL_checkinteger(state, 2);
    if (row < 1 || static_cast<std::uint64_t>(row) > impl->peerCount) {
        return luaL_error(state, "peer row is outside the published set");
    }
    push_peer_view(state, static_cast<std::uint8_t>(row - 1));
    return 1;
}

/** context.peers members in table order. */
constexpr std::array<Binding<Impl>, std::size(api::kPeersMembers)> kPeersBindings{{
    {"count",
     nullptr,
     [](lua_State* state, const Impl& impl) { lua_pushinteger(state, impl.peerCount); }},
    {"at", &peer_collection_at},
}};
static_assert(bound_to<Impl>(api::kPeersMembers, kPeersBindings));

/** Reads count, or the accessor for one peer. */
[[nodiscard]] int peer_collection_index(lua_State* state) {
    static_cast<void>(luaL_checkudata(state, 1, kPeerCollectionMetatable));
    const Impl* const impl = impl_from_state(state);
    return dispatch<Impl>(state, kPeersBindings, lua_string_view(state, 2), *impl);
}

/** Peer members in table order. All four are 64-bit, so they cross as decimal strings. */
constexpr std::array<Binding<PeerSession>, std::size(api::kPeerMembers)> kPeerBindings{{
    {"session_id",
     nullptr,
     [](lua_State* state, const PeerSession& peer) { push_u64_string(state, peer.sessionId); }},
    {"session_generation",
     nullptr,
     [](lua_State* state, const PeerSession& peer) {
         push_u64_string(state, peer.sessionGeneration);
     }},
    {"member_key",
     nullptr,
     [](lua_State* state, const PeerSession& peer) { push_u64_string(state, peer.memberKey); }},
    {"join_identity",
     nullptr,
     [](lua_State* state, const PeerSession& peer) { push_u64_string(state, peer.joinIdentity); }},
}};
static_assert(bound_to<PeerSession>(api::kPeerMembers, kPeerBindings));

/** Reads one member of a peer session. */
[[nodiscard]] int peer_view_index(lua_State* state) {
    const auto* const handle =
        static_cast<const PeerViewHandle*>(luaL_checkudata(state, 1, kPeerViewMetatable));
    const PeerSession& peer = current_peer(state, *handle);
    return dispatch<PeerSession>(state, kPeerBindings, lua_string_view(state, 2), peer);
}

} // namespace

void push_peers(lua_State* state) {
    push_handle(state, kPeerCollectionMetatable, PeerCollectionHandle{});
}

void push_peer_view(lua_State* state, std::uint8_t row) {
    const auto& peer = impl_from_state(state)->peers[row];
    push_handle(
        state, kPeerViewMetatable, PeerViewHandle{peer.sessionId, peer.sessionGeneration, row});
}

void register_peer_metatables(lua_State* state) {
    register_metatable(state, kPeerCollectionMetatable, &peer_collection_index);
    register_metatable(state, kPeerViewMetatable, &peer_view_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
