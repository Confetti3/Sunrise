#pragma once

#include <cstdint>
#include <string_view>

#include "../definition.h"

namespace sunrise::state::mods::manifest {

/** Readable reason a manifest was refused. */
using Error = Text<kProblemCapacity>;

/** @return True for a lowercase reverse-domain id such as `dev.projectsunrise.example`. */
[[nodiscard]] bool valid_id(std::string_view id) noexcept;

/** @return True for a config key: 1 to 32 of `a-z`, `0-9` and `_`, starting with a letter. */
[[nodiscard]] bool valid_config_key(std::string_view key) noexcept;

/**
 * Reads a config type name: `int`, `float` or `bool`.
 * @param name The name as the manifest spells it.
 * @param output Receives the type; unchanged on failure.
 * @return False for any other name.
 */
[[nodiscard]] bool config_type_name(std::string_view name, ConfigType& output) noexcept;

/**
 * Reads one JSON value as a config type. An `int` must be a whole number that fits 32 bits, a
 * `float` any finite number that fits a float, a `bool` true or false.
 * @param json The value text and nothing else.
 * @param output Its `type` picks the reading. Receives the value; changed only on success.
 * @return False when the text is not one value of that type.
 */
[[nodiscard]] bool read_config_value(std::string_view json, ConfigValue& output) noexcept;

/**
 * Reads only the root `manifestVersion`, so a newer format is named before its keys are judged.
 * @param json Complete file text.
 * @param output Receives the version, or zero when the key is absent.
 * @return False when the text is not a JSON object.
 */
[[nodiscard]] bool read_version(std::string_view json, std::uint32_t& output) noexcept;

/**
 * Parses one `mod.json` of a supported version and checks every field rule.
 * Unknown and repeated keys are refused, so a misspelt key cannot drop a dependency.
 * @param json Complete file text.
 * @param output Receives the manifest. Only valid on success.
 * @param error Receives a readable reason on failure.
 * @return True when the manifest is complete and valid.
 */
[[nodiscard]] bool parse(std::string_view json, Manifest& output, Error& error) noexcept;

} // namespace sunrise::state::mods::manifest
