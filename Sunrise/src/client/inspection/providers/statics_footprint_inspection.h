#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../world_inspection_model.h"

namespace sunrise::client::inspection::providers::statics_footprints {

struct AppendResult final {
    NodeId groupNode{};
    std::size_t published{};
    std::size_t rejected{};
    std::size_t truncated{};
    bool present{};
};

/**
 * Appends one node per published package-derived statics footprint: a
 * catalog-provenance world AABB with a center transform, grouped under one
 * "Static footprints" parent. Rows arrive empty until the extraction pass
 * publishes, so this provider re-appends nothing until then.
 */
[[nodiscard]] AppendResult
append(Graph& graph, std::vector<Diagnostic>& diagnostics, const Source& source, NodeId parent);

} // namespace sunrise::client::inspection::providers::statics_footprints
