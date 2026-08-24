#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::inspection::bubble_catalog {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::uint32_t kTargetContentBuild = 86657;
inline constexpr std::string_view kTargetContentBuildText = "86657";
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kRecordSize = 64;
inline constexpr std::size_t kFamilyCapacity = 32;
inline constexpr std::size_t kMaximumBubbles = 4096;

/** One package-derived map-bubble footprint. */
struct Bubble final {
    std::uint64_t tag{};
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    std::string family;
};

struct Catalog final {
    std::uint32_t contentBuild{};
    std::string family;
    std::vector<Bubble> bubbles;
};

enum class Compatibility : std::uint8_t {
    missing,
    malformed,
    buildMismatch,
    compatible,
};

struct LoadResult final {
    Compatibility compatibility{Compatibility::missing};
    std::string diagnostic;
};

[[nodiscard]] bool validate(const Catalog& catalog, std::string& error);
[[nodiscard]] bool load(std::span<const std::byte> bytes, Catalog& catalog, std::string& error);
[[nodiscard]] LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept;
[[nodiscard]] Compatibility compatibility(const Catalog& catalog) noexcept;

} // namespace sunrise::client::inspection::bubble_catalog
