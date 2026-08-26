#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../inspection/activity_logic_catalog.h"

namespace sunrise::client::content::activity::logic_packages {

using Cancelled = bool (*)(void* context) noexcept;

struct Progress final {
    std::size_t resources{};
    std::size_t definitions{};
    std::size_t mapRows{};
    std::size_t publishedPlacements{};
    std::size_t references{};
    std::size_t rejected{};
};

/**
 * Builds one scenario's Activity Logic catalogue by following only validated package-tag
 * dependencies rooted at that scenario. No installed-package class sweep is performed.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch,
                         std::uint32_t scenarioTag,
                         std::string_view mapFamily,
                         std::span<const std::byte> contentFingerprint,
                         Cancelled cancelled,
                         void* cancelContext,
                         inspection::activity_logic_catalog::Catalog& output,
                         Progress& progress) noexcept;

} // namespace sunrise::client::content::activity::logic_packages
