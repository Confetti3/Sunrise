#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::inspection::activity_logic_catalog {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::size_t kDigestSize = 32;
inline constexpr std::size_t kHeaderSize = 160;
using Digest = std::array<std::uint8_t, kDigestSize>;

enum class Role : std::uint8_t {
    actionSequence = 0,
    actionTarget = 1,
    competitiveRule = 2,
    conditionMonitor = 3,
    device = 4,
    object = 5,
    objective = 6,
    spatialRule = 7,
    spawnDefinition = 8,
    squadDefinition = 9,
    triggerSource = 10,
    unknown = 255,
};

enum class Confidence : std::uint8_t {
    unknown = 0,
    probable = 1,
    strong = 2,
};

struct Placement final {
    std::uint64_t worldId{};
    std::uint32_t mapTableTag{};
    std::uint32_t placedEntityTag{};
    std::array<float, 3> position{};
    std::array<float, 4> rotation{};
};

struct Entity final {
    std::uint32_t definitionTag{};
    std::uint32_t classPrimary{};
    std::uint32_t classSecondary{};
    Role role{Role::unknown};
    Confidence confidence{Confidence::unknown};
    std::string name;
    std::string label;
    std::string localizedText;
    std::vector<Placement> placements;
};

struct Activity final {
    std::uint32_t scenarioTag{};
    std::string name;
    std::string destination;
    std::vector<std::uint32_t> entityIndices;
};

struct Edge final {
    std::uint32_t sourceEntityIndex{};
    std::uint32_t targetEntityIndex{};
    std::uint32_t nameHash{};
    std::uint32_t occurrenceCount{};
};

struct Catalog final {
    Digest sourceDigest{};
    std::vector<Activity> activities;
    std::vector<Entity> entities;
    std::vector<Edge> edges;
};

enum class LoadState : std::uint8_t {
    missing,
    malformed,
    ready,
};

struct LoadResult final {
    LoadState state{LoadState::missing};
    std::string diagnostic;
};

[[nodiscard]] bool validate(const Catalog& catalog, std::string& error);
[[nodiscard]] bool load(std::span<const std::byte> bytes, Catalog& catalog, std::string& error);
[[nodiscard]] LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept;
[[nodiscard]] const Activity* find_activity(const Catalog& catalog,
                                            std::uint32_t scenarioTag) noexcept;
[[nodiscard]] const Entity* find_entity(const Catalog& catalog,
                                        std::uint32_t definitionTag) noexcept;
[[nodiscard]] const char* role_name(Role role) noexcept;
[[nodiscard]] const char* confidence_name(Confidence confidence) noexcept;

} // namespace sunrise::client::inspection::activity_logic_catalog
