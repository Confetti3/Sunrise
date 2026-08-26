#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../inspection/activity_graph_catalog.h"

namespace sunrise::client::content::activity::graph_packages {

using Cancelled = bool (*)(void* context) noexcept;

struct Progress final {
    std::size_t activities{};
    std::size_t graphs{};
    std::size_t nodes{};
    std::size_t states{};
    std::size_t links{};
    std::size_t rejected{};
    std::string diagnostic;
};

/**
 * Builds the bounded Activity Graph shard associated with one current scenario.
 * Package 0x0593 is the proven fixed dependency table; no package or class sweep is performed.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch,
                         std::uint32_t scenarioTag,
                         std::string_view mapFamily,
                         std::span<const std::byte> contentFingerprint,
                         Cancelled cancelled,
                         void* cancelContext,
                         inspection::activity_catalog::Catalog& output,
                         Progress& progress) noexcept;

} // namespace sunrise::client::content::activity::graph_packages
