#pragma once

namespace sunrise::client::hooks::package_trust {

/**
 * Accepts package RSA, extended-header hash and cached-data hash authentication. Native package
 * parsing, decompression, file-size, table and bounds validation remain active.
 * @return True when the validator detour and both code patches are in place.
 */
[[nodiscard]] bool install() noexcept;

/** @return True when both code patches are restored and the validator detour is detached. */
[[nodiscard]] bool uninstall() noexcept;

} // namespace sunrise::client::hooks::package_trust
