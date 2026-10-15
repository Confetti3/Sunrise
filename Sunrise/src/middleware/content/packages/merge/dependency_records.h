#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "merge.h"

namespace sunrise::middleware::content::packages::merge::dependency_records {

/**
 * Unions the dependency records of several patches of one base misc region.
 * Patches may only add entries to records; every other byte must match the base.
 * @param base Misc region of the base generation.
 * @param patches Misc regions of the patches, in load order.
 * @param output Receives the merged misc region; the base when no patch changes it.
 * @return ok, names when a patch changes anything but added entries, or capacity.
 */
[[nodiscard]] Status merge(std::span<const std::byte> base,
                           std::span<const std::span<const std::byte>> patches,
                           std::vector<std::byte>& output) noexcept;

} // namespace sunrise::middleware::content::packages::merge::dependency_records
