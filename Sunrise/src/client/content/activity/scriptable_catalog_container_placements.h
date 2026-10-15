#pragma once

#include <memory>
#include <string_view>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/scriptables/definition.h"
#include "scriptable_catalog_container_index.h"

namespace sunrise::client::content::scenarios {
class LayoutSet;
} // namespace sunrise::client::content::scenarios

namespace sunrise::client::content::activity::scriptables::internal {

using ContainerPlacementCancelCheck = bool (*)() noexcept;

/** Complete object-list graphs reused during one sequential package-estate pass. */
class ContainerPlacementCache final {
public:
    ContainerPlacementCache() noexcept;
    ~ContainerPlacementCache();

    ContainerPlacementCache(const ContainerPlacementCache&) = delete;
    ContainerPlacementCache& operator=(const ContainerPlacementCache&) = delete;
    ContainerPlacementCache(ContainerPlacementCache&&) = delete;
    ContainerPlacementCache& operator=(ContainerPlacementCache&&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};

    friend struct ContainerPlacementCacheAccess;
};

/**
 * Appends the selected-stem container placement graph without assigning ClientRef identity.
 * @param layouts Destination layouts to read the stem from, or null for the ones State publishes.
 */
[[nodiscard]] bool
append_container_placements(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            const ContainerIndex& containers,
                            const ::sunrise::client::content::scenarios::LayoutSet* layouts,
                            std::string_view scenarioName,
                            state::build_data::scriptables::Snapshot& output,
                            ContainerPlacementCancelCheck cancel) noexcept;

/**
 * Appends container placements with exact pass-local object-list graph reuse.
 * @param layouts Destination layouts to read the stem from, or null for the ones State publishes.
 */
[[nodiscard]] bool
append_container_placements(const middleware::content::packages::reader::Source& source,
                            middleware::content::packages::reader::Scratch& scratch,
                            ContainerPlacementCache& cache,
                            const ContainerIndex& containers,
                            const ::sunrise::client::content::scenarios::LayoutSet* layouts,
                            std::string_view scenarioName,
                            state::build_data::scriptables::Snapshot& output,
                            ContainerPlacementCancelCheck cancel) noexcept;

} // namespace sunrise::client::content::activity::scriptables::internal
