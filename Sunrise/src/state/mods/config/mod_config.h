#pragma once

#include <span>
#include <string_view>

#include "../definition.h"

namespace sunrise::state::mods::config {

/**
 * Loads the saved values from `mod_config.json` in the Sunrise folder, once at boot.
 * A refused file is moved to `mod_config.json.bad`; if that fails, saving stays off.
 * @param artifactDirectory The Sunrise folder, without a trailing separator.
 * @return False when the file path does not fit.
 */
[[nodiscard]] bool load(std::wstring_view artifactDirectory) noexcept;

/**
 * Records the value each bound key of a loaded mod used for the package bake.
 * Runs once, after the overlay is applied, while the mods lock is held.
 * @param records Every record of the boot. Only loaded ones count.
 */
void publish(std::span<const Record> records) noexcept;

/** Reads a bound key's startup value without visiting the mod list. */
[[nodiscard]] bool
applied(std::string_view modId, std::string_view key, ConfigValue& output) noexcept;

/** Compares the current bound value to the exact typed value used by loaded packages. */
[[nodiscard]] bool restart_required(std::string_view modId, const ConfigEntry& entry) noexcept;

/** Floats compare by bits, including the sign of zero; unused value members are ignored. */
[[nodiscard]] bool same_value(const ConfigValue& first, const ConfigValue& second) noexcept;

/** Drops the saved values and the file path. */
void shutdown() noexcept;

/**
 * Current value of one declared key of a loaded mod; the default when none is stored.
 * Visits the mod list, so never call it from inside a `mods::visit` callback.
 * @param modId Id of a loaded mod.
 * @param key Key the mod declares.
 * @param output Receives the value; unchanged on failure.
 * @return False when the mod is not loaded or does not declare the key.
 */
[[nodiscard]] bool read(std::string_view modId, std::string_view key, ConfigValue& output) noexcept;

/**
 * Stores and saves one value. Visits the mod list, so never call it from a `mods::visit` callback.
 * @param modId Id of a loaded mod.
 * @param key Key the mod declares.
 * @param value New value, of the declared type.
 * @return False on invalid input or save failure; failed saves leave the prior value unchanged.
 */
[[nodiscard]] bool
write(std::string_view modId, std::string_view key, const ConfigValue& value) noexcept;

/**
 * Removes the saved value, so the key follows its default again. Visits the mod list.
 * @param modId Id of a loaded mod.
 * @param key Key the mod declares.
 * @return False on an undeclared key or a failed save.
 */
[[nodiscard]] bool reset(std::string_view modId, std::string_view key) noexcept;

/**
 * Value for a declaration the caller already holds. It never visits the mod list, so a
 * `mods::visit` callback may call it.
 * @param modId Id of the mod that declares `entry`.
 * @param entry The declaration.
 * @param output Receives the saved value, or the default.
 * @return True when a saved value applies; false when `output` holds the default.
 */
[[nodiscard]] bool
current(std::string_view modId, const ConfigEntry& entry, ConfigValue& output) noexcept;

} // namespace sunrise::state::mods::config
