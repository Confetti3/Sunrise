#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../activity_logic_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::activity_logic {

struct State final {
    activity_logic_catalog::Catalog catalog;
    activity_logic_catalog::LoadResult load;
    std::string reloadDiagnostic;
    void* module{};
    bool initialized{};
};

struct AppendResult final {
    NodeId root{};
    bool present{};
    bool matched{};
    bool browseOnly{};
    std::uint32_t scenarioTag{};
    std::uint32_t definitionCount{};
    std::uint32_t placementCount{};
    std::string activityName;
    std::string destination;
    std::string diagnostic;
};

/** Catalog-level list of browsable activities; empty when no catalog is loaded. */
struct BrowseSummary final {
    std::uint32_t scenarioTag{};
    std::string activityName;
    std::string destination;
};

[[nodiscard]] std::vector<BrowseSummary> browse_activities() noexcept;

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] bool reload() noexcept;
[[nodiscard]] const State& state() noexcept;

[[nodiscard]] AppendResult append(Graph& graph,
                                  std::vector<Diagnostic>& diagnostics,
                                  const Source& source,
                                  NodeId parent);

/** Appends one specific activity without requiring a live scenario match. */
[[nodiscard]] AppendResult append_browse(Graph& graph,
                                         std::vector<Diagnostic>& diagnostics,
                                         std::uint32_t scenarioTag,
                                         NodeId parent);

} // namespace sunrise::client::inspection::providers::activity_logic
