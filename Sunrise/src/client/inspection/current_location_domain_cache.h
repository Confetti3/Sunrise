#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "activity_logic_catalog.h"
#include "activity_graph_catalog.h"
#include "bubble_bounds_catalog.h"

namespace sunrise::client::inspection::current_location_domain_cache {

enum class LoadState : std::uint8_t { missing, stale, rejected, ready };

struct LoadResult final {
    LoadState state{LoadState::missing};
    std::string diagnostic;
};

[[nodiscard]] LoadResult load_activity_graph(std::wstring_view path,
                                             std::uint32_t scenarioTag,
                                             std::span<const std::byte> contentFingerprint,
                                             activity_catalog::Catalog& output) noexcept;
[[nodiscard]] bool store_activity_graph_atomic(std::wstring_view path,
                                               const activity_catalog::Catalog& catalog,
                                               std::uint32_t scenarioTag,
                                               std::span<const std::byte> contentFingerprint,
                                               std::string& diagnostic) noexcept;

[[nodiscard]] LoadResult load_activity_logic(std::wstring_view path,
                                             std::uint32_t scenarioTag,
                                             std::span<const std::byte> contentFingerprint,
                                             activity_logic_catalog::Catalog& output) noexcept;
[[nodiscard]] bool store_activity_logic_atomic(std::wstring_view path,
                                               const activity_logic_catalog::Catalog& catalog,
                                               std::uint32_t scenarioTag,
                                               std::span<const std::byte> contentFingerprint,
                                               std::string& diagnostic) noexcept;

[[nodiscard]] LoadResult load_bubble_bounds(std::wstring_view path,
                                            std::string_view family,
                                            bubble_catalog::Catalog& output) noexcept;
[[nodiscard]] bool store_bubble_bounds_atomic(std::wstring_view path,
                                              const bubble_catalog::Catalog& catalog,
                                              std::string_view family,
                                              std::string& diagnostic) noexcept;

} // namespace sunrise::client::inspection::current_location_domain_cache
