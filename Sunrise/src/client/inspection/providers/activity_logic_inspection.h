#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../activity_logic_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::activity_logic {

struct State final {
    activity_logic_catalog::Catalog locationCatalog;
    std::uint64_t publicationRevision{};
    std::uint32_t locationScenarioTag{};
    Source activationSource;
    bool locationActive{};
};

struct AppendResult final {
    NodeId root{};
    bool present{};
    bool matched{};
    std::uint32_t scenarioTag{};
    std::uint32_t definitionCount{};
    std::uint32_t placementCount{};
    std::string activityName;
    std::string destination;
    std::string diagnostic;
};

/** Activates one validated current-location shard. */
[[nodiscard]] bool activate_location(activity_logic_catalog::Catalog catalog,
                                     Source source) noexcept;
void deactivate_location() noexcept;
[[nodiscard]] std::uint64_t publication_revision() noexcept;

[[nodiscard]] const State& state() noexcept;

[[nodiscard]] AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent);

} // namespace sunrise::client::inspection::providers::activity_logic
