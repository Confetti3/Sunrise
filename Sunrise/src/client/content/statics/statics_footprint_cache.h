#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../state/content_manifest/definition.h"
#include "statics_footprints.h"

namespace sunrise::client::content::statics::cache {

// Version 2 binds collection to the scenario package's exact content family rather than a
// canonical destination prefix that can also match unrelated retained packages.
inline constexpr std::uint32_t kSchemaVersion = 2;

struct Key final {
    state::content_manifest::Fingerprint contentFingerprint{};
    std::uint32_t scenarioTag{};
    std::string mapFamily;
    std::array<std::uint16_t, 8> packageIds{};
    std::size_t packageCount{};
};

enum class LoadState : std::uint8_t { missing, rejected, ready };

struct LoadResult final {
    LoadState state{LoadState::missing};
    std::string diagnostic;
};

/** Loads and validates one exact current-location footprint shard. */
[[nodiscard]] LoadResult load(std::wstring_view path,
                              const Key& key,
                              std::vector<Footprint>& rows,
                              Progress& progress) noexcept;

/** Writes, reload-validates, and atomically replaces one footprint shard. */
[[nodiscard]] bool store_atomic(std::wstring_view path,
                                const Key& key,
                                std::span<const Footprint> rows,
                                const Progress& progress,
                                std::string& diagnostic) noexcept;

} // namespace sunrise::client::content::statics::cache
