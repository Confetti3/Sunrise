#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../../../state/build_data/hash_names/definition.h"
#include "../../../state/build_data/scriptables/definition.h"
#include "../../../state/content/content_catalog.h"

namespace sunrise::client::content::activity::scriptables::internal {

/** One bounded inline package string retained with its exact source definition. */
struct InlineName final {
    std::uint32_t hash{};
    std::uint32_t sourceTag{};
    std::string value{};
};

using NameCancelCheck = bool (*)() noexcept;

/**
 * Resolves authored hashes and exact installed-package definition tags in one snapshot.
 * @param installed Named package definitions, or null for the catalog State publishes.
 * @param hashNames Identifier names sorted by hash, or null for the table State publishes.
 */
[[nodiscard]] bool resolve_names(state::build_data::scriptables::Snapshot& output,
                                 std::span<const InlineName> inlineNames,
                                 const std::vector<state::content::Definition>* installed,
                                 const std::vector<state::build_data::hash_names::Name>* hashNames,
                                 NameCancelCheck cancel) noexcept;

} // namespace sunrise::client::content::activity::scriptables::internal
