#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::inspection::activity_catalog {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::uint32_t kTargetContentBuild = 86657;
inline constexpr std::size_t kDigestSize = 32;
inline constexpr std::size_t kHeaderSize = 224;
inline constexpr std::string_view kTargetContentBuildText = "86657";
using Digest = std::array<std::uint8_t, kDigestSize>;

struct Activity final {
    std::uint32_t hash{};
    std::string name;
    std::vector<std::uint32_t> graphHashes;
};

struct GraphNode final {
    std::uint32_t graphHash{};
    std::uint32_t nodeHash{};
    float authoredX{};
    float authoredY{};
    std::uint32_t stateHash{};
    std::uint32_t styleHash{};
    std::vector<std::uint32_t> activityHashes;
    std::vector<std::uint32_t> linkedGraphHashes;
};

struct Graph final {
    std::uint32_t hash{};
    std::vector<GraphNode> nodes;
    std::vector<std::uint32_t> linkedGraphHashes;
};

struct LocationRelease final {
    std::uint32_t locationHash{};
    std::uint32_t graphHash{};
    std::uint32_t nodeHash{};
    std::array<float, 3> spawnPoint{};
    std::array<float, 4> publicPosition{};
};

struct Catalog final {
    std::uint32_t contentBuild{};
    std::string manifestVersion;
    Digest activityDigest{};
    Digest graphDigest{};
    Digest locationDigest{};
    std::vector<Activity> activities;
    std::vector<Graph> graphs;
    std::vector<LocationRelease> locationReleases;
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
[[nodiscard]] bool load(std::span<const std::byte> bytes,
                        Catalog& catalog,
                        std::string& error);
[[nodiscard]] LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept;
[[nodiscard]] Compatibility compatibility(const Catalog& catalog) noexcept;
[[nodiscard]] const Activity* find_activity(const Catalog& catalog, std::uint32_t hash) noexcept;
[[nodiscard]] const Graph* find_graph(const Catalog& catalog, std::uint32_t hash) noexcept;
[[nodiscard]] const GraphNode* find_node(const Graph& graph, std::uint32_t nodeHash) noexcept;

} // namespace sunrise::client::inspection::activity_catalog
