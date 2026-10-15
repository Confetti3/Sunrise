#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/hash_names/definition.h"
#include "../../../state/build_data/scenarios/definition.h"

namespace sunrise::client::content::hash_names {

/**
 * Finds the internal name of every destination bubble it can, once.
 * A bubble stores only the hash of its name, so names are recovered by hashing the ids the
 * installed objects carry. Runs after the destination layouts publish, which give the hashes.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the item build.
 * @return True when State already holds the table or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

/**
 * Names the bubble and spawn-set hashes from installed packages only, in one call.
 * Publishes nothing. Waits for the parallel reader, so never call it from a boot slice.
 * @param source Package directory and block keys; mod files are ignored whatever it says.
 * @param definitions Installed destination layouts whose bubble hashes are named.
 * @param output Receives the named rows in ascending hash order; empty on failure.
 * @return True when the whole pass ran, or when there was nothing to name.
 */
[[nodiscard]] bool
build_private(const middleware::content::packages::reader::Source& source,
              std::span<const state::build_data::scenarios::Definition> definitions,
              std::vector<state::build_data::hash_names::Name>& output) noexcept;

} // namespace sunrise::client::content::hash_names
