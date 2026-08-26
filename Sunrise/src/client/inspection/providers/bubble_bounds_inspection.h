#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../bubble_bounds_catalog.h"
#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::bubble_bounds {

struct State final {
    bubble_catalog::Catalog locationCatalog;
    std::string locationFamily;
    std::uint64_t publicationRevision{};
    bool locationActive{};
};

struct AppendResult final {
    NodeId groupNode{};
    bool present{};
    std::uint32_t contentBuild{};
    std::size_t bubbleCount{};
    std::string diagnostic;
};

[[nodiscard]] const State& state() noexcept;
[[nodiscard]] bool activate_location(bubble_catalog::Catalog catalog,
                                     std::string_view family) noexcept;
void deactivate_location() noexcept;
[[nodiscard]] std::uint64_t publication_revision() noexcept;

/**
 * Appends one node per package-derived map-bubble footprint: a catalog-provenance
 * AABB with a center transform. Bubbles never claim object-level shapes; they are
 * the aggregate static+terrain footprint of one playable space.
 */
[[nodiscard]] AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent);

} // namespace sunrise::client::inspection::providers::bubble_bounds
