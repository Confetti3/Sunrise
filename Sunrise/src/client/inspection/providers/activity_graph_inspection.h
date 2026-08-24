#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../activity_graph_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::activity_graph {

struct State final {
    activity_catalog::Catalog catalog;
    activity_catalog::LoadResult load;
    bool initialized{};
};

struct AppendResult final {
    NodeId catalogNode{};
    bool present{};
    bool buildMatch{};
    std::uint32_t contentBuild{};
    std::string version;
    std::string diagnostic;
};

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] const State& state() noexcept;

[[nodiscard]] AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent);

} // namespace sunrise::client::inspection::providers::activity_graph
