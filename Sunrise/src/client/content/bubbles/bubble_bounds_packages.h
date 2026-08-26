#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../inspection/bubble_bounds_catalog.h"

namespace sunrise::client::content::bubbles::packages {

using Cancelled = bool (*)(void* context) noexcept;

struct Progress final {
    std::size_t scenarioBubbles{};
    std::size_t parents{};
    std::size_t placements{};
    std::size_t published{};
    std::size_t rejected{};
    std::size_t hashPackages{};
    std::size_t emptyDependencies{};
    std::string diagnostic;
};

/** Builds package-native bounds for the current scenario's bubbles. */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch,
                         std::uint32_t scenarioTag,
                         std::string_view mapFamily,
                         Cancelled cancelled,
                         void* cancelContext,
                         inspection::bubble_catalog::Catalog& output,
                         Progress& progress) noexcept;

} // namespace sunrise::client::content::bubbles::packages
