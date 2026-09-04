#pragma once

namespace sunrise::client::hooks::peer_relay {

/** Attaches the peer-channel direct-connect forcing. @return True when the detour attaches. */
[[nodiscard]] bool install() noexcept;

/** Detaches the peer-channel direct-connect forcing. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::peer_relay
