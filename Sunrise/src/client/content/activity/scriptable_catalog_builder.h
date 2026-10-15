#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/hash_names/definition.h"
#include "../../../state/build_data/scriptables/definition.h"
#include "../../../state/content/content_catalog.h"
#include "scriptable_catalog_container_index.h"

namespace sunrise::client::content::scenarios {
class LayoutSet;
} // namespace sunrise::client::content::scenarios

namespace sunrise::client::content::activity::scriptables::internal {

using BuilderCancelCheck = bool (*)() noexcept;

/** Package-wide tables one scenario build reads. A null member reads the table State publishes. */
struct BuildTables final {
    const ::sunrise::client::content::scenarios::LayoutSet* layouts{};
    const std::vector<state::content::Definition>* definitions{};
    /** Identifier names, sorted by hash. */
    const std::vector<state::build_data::hash_names::Name>* hashNames{};
};

/**
 * Immutable source analyses shared by scenario builds from one installed package estate.
 * Use one instance for one sequential pass. Destroy it before changing the source estate.
 */
class ScenarioAnalysisCache final {
public:
    ScenarioAnalysisCache() noexcept;
    ~ScenarioAnalysisCache();

    ScenarioAnalysisCache(const ScenarioAnalysisCache&) = delete;
    ScenarioAnalysisCache& operator=(const ScenarioAnalysisCache&) = delete;
    ScenarioAnalysisCache(ScenarioAnalysisCache&&) = delete;
    ScenarioAnalysisCache& operator=(ScenarioAnalysisCache&&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};

    friend std::shared_ptr<state::build_data::scriptables::Snapshot>
    build_scenario_catalog(const middleware::content::packages::reader::Source& source,
                           const ContainerIndex& containers,
                           middleware::content::packages::reader::Scratch& scratch,
                           ScenarioAnalysisCache& analyses,
                           const BuildTables& tables,
                           std::uint32_t scenarioTag,
                           std::string_view scenarioName,
                           BuilderCancelCheck cancel);
};

/**
 * Builds one immutable package-derived scenario catalog without publishing it.
 * The caller owns package-key lifetime for the complete call and may attach its own request or
 * revision identity after the returned snapshot is complete.
 */
[[nodiscard]] std::shared_ptr<state::build_data::scriptables::Snapshot>
build_scenario_catalog(const middleware::content::packages::reader::Source& source,
                       const ContainerIndex& containers,
                       middleware::content::packages::reader::Scratch& scratch,
                       std::uint32_t scenarioTag,
                       std::string_view scenarioName,
                       BuilderCancelCheck cancel);

/**
 * Builds one scenario while reusing source-only object analysis from earlier pass rows.
 * @param tables Layouts and names the build reads in place of the tables State publishes.
 */
[[nodiscard]] std::shared_ptr<state::build_data::scriptables::Snapshot>
build_scenario_catalog(const middleware::content::packages::reader::Source& source,
                       const ContainerIndex& containers,
                       middleware::content::packages::reader::Scratch& scratch,
                       ScenarioAnalysisCache& analyses,
                       const BuildTables& tables,
                       std::uint32_t scenarioTag,
                       std::string_view scenarioName,
                       BuilderCancelCheck cancel);

} // namespace sunrise::client::content::activity::scriptables::internal
