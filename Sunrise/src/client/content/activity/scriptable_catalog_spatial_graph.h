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

using SpatialCancelCheck = bool (*)() noexcept;

/** Owns complete source tables reused within one sequential package pass. */
class StaticSpatialCache final {
public:
    StaticSpatialCache() noexcept;
    ~StaticSpatialCache();

    StaticSpatialCache(const StaticSpatialCache&) = delete;
    StaticSpatialCache& operator=(const StaticSpatialCache&) = delete;
    StaticSpatialCache(StaticSpatialCache&&) = delete;
    StaticSpatialCache& operator=(StaticSpatialCache&&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};

    friend struct StaticSpatialCacheAccess;
};

/**
 * Appends package spatial candidates whose stem and bubble mask match this scenario.
 * @param layouts Destination layouts to read the stem from, or null for the ones State publishes.
 */
[[nodiscard]] bool
append_static_spatial_candidates(const middleware::content::packages::reader::Source& source,
                                 middleware::content::packages::reader::Scratch& scratch,
                                 const ContainerIndex& containers,
                                 const ::sunrise::client::content::scenarios::LayoutSet* layouts,
                                 std::string_view scenarioName,
                                 state::build_data::scriptables::Snapshot& output,
                                 SpatialCancelCheck cancel) noexcept;

/**
 * Appends static-spatial rows while reusing complete tables from this package pass.
 * @param layouts Destination layouts to read the stem from, or null for the ones State publishes.
 */
[[nodiscard]] bool
append_static_spatial_candidates(const middleware::content::packages::reader::Source& source,
                                 middleware::content::packages::reader::Scratch& scratch,
                                 StaticSpatialCache& cache,
                                 const ContainerIndex& containers,
                                 const ::sunrise::client::content::scenarios::LayoutSet* layouts,
                                 std::string_view scenarioName,
                                 state::build_data::scriptables::Snapshot& output,
                                 SpatialCancelCheck cancel) noexcept;

} // namespace sunrise::client::content::activity::scriptables::internal
