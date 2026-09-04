#pragma once

#include "../../../patterns/image_scan.h"

namespace sunrise::client::hooks::network::investment {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Attaches the family-five commit rearm.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_family5_rearm() noexcept;

/** @return True when the family-five commit rearm is absent. */
[[nodiscard]] bool uninstall_family5_rearm() noexcept;

/** @return True while the family-five commit rearm is attached. */
[[nodiscard]] bool family5_rearm_is_installed() noexcept;

/** Moves four mislabeled Arrivals members between their validated native reusable plug sets. */
void apply_socket_menu_routing() noexcept;

/** Reserves low-address storage before retail content occupies the compatible address domain. */
void reserve_socket_menu_routing_storage() noexcept;

/** Arms socket-menu correction at native content-table patch completion. */
void arm_socket_menu_routing() noexcept;

/** Restores native plug-set descriptors changed by the Arrivals membership correction. */
void restore_socket_menu_routing() noexcept;

/**
 * Arms one derived-state rebuild after replicated investment state changes.
 */
void arm_derived_rebuild() noexcept;

} // namespace sunrise::client::hooks::network::investment
