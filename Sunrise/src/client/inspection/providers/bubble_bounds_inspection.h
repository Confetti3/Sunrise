#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../bubble_bounds_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::bubble_bounds {

struct State final {
    bubble_catalog::Catalog catalog;
    bubble_catalog::LoadResult load;
    bool initialized{};
};

struct AppendResult final {
    NodeId groupNode{};
    bool present{};
    bool buildMatch{};
    std::uint32_t contentBuild{};
    std::size_t bubbleCount{};
    std::string diagnostic;
};

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] const State& state() noexcept;

/**
 * Appends one node per package-derived map-bubble footprint: a catalog-provenance
 * AABB with a center transform. Bubbles never claim object-level shapes; they are
 * the aggregate static+terrain footprint of one playable space.
 */
[[nodiscard]] AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent);

} // namespace sunrise::client::inspection::providers::bubble_bounds
