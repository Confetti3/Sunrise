#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../activity_graph_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::activity_graph {

struct State final {
    activity_catalog::Catalog locationCatalog;
    std::uint64_t publicationRevision{};
    std::uint32_t locationScenarioTag{};
    Source activationSource;
    bool locationActive{};
};

struct AppendResult final {
    NodeId catalogNode{};
    bool present{};
    std::uint32_t contentBuild{};
    std::uint32_t collectorVersion{};
    std::string diagnostic;
};

[[nodiscard]] const State& state() noexcept;

/** Activates one validated current-location shard. */
[[nodiscard]] bool activate_location(activity_catalog::Catalog catalog,
                                     Source source) noexcept;
void deactivate_location() noexcept;
[[nodiscard]] std::uint64_t publication_revision() noexcept;

[[nodiscard]] AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent);

} // namespace sunrise::client::inspection::providers::activity_graph
