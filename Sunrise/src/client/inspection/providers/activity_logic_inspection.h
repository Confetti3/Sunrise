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
    bool initialized{};
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

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] const State& state() noexcept;

[[nodiscard]] AppendResult append(Graph& graph,
                                  std::vector<Diagnostic>& diagnostics,
                                  const Source& source,
                                  NodeId parent);

} // namespace sunrise::client::inspection::providers::activity_logic
