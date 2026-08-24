#pragma once

namespace sunrise::state::build_data::records::rewards {

/**
 * Keeps the manifest-sourced record-reward table in its own file, beside settings.json.
 *
 * This is shipped, generated data -- an external tool joins Bungie's manifest by record hash and
 * writes the file -- not extraction-cache output, so it does not live under the cache directory
 * the record and node tables use, and nothing here ever writes it back out. The settings-authored
 * `record_rewards` table remains the operator's override and is tried first; see
 * `state::account::find_record_reward` and `find_generated_record_reward` for the precedence.
 */

/**
 * Derives the reward file path next to the executable's other shipped data.
 * @param module Loaded DLL, used to find the artifact directory.
 * @return True when the path resolves. Loading is best effort and reported separately.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/**
 * Loads the shipped reward table and publishes it, if one is installed.
 * A missing or empty file is a clean no-op: the feature simply stays inert. Logs one informational
 * line reporting rows loaded, distinct records covered, and item hashes that failed to resolve
 * against this build's item table.
 * @return True when the file was absent, empty, or loaded and published without error.
 */
[[nodiscard]] bool load_and_publish() noexcept;

} // namespace sunrise::state::build_data::records::rewards
