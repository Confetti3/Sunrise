#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../hash_names/definition.h"
#include "../scenarios/definition.h"
#include "../spawn_sets/definition.h"

namespace sunrise::state::build_data::worlds {

inline constexpr std::size_t kWorldCapacity = scenarios::kDefinitionCapacity;
inline constexpr std::size_t kBubbleCapacity = scenarios::kBubbleCapacity;
inline constexpr std::size_t kSliceCapacity = 8;
inline constexpr std::size_t kSpawnCapacity = 1'024;

struct Summary final {
    std::array<char, scenarios::kNameCapacity> name{};
    std::array<char, scenarios::kSpawnStemCapacity> mapStem{};
    std::uint32_t scenarioTag{};
    std::uint8_t nameLength{};
    std::uint8_t mapStemLength{};
    std::uint8_t bubbleCount{};
    bool truncated{};
};

struct Bubble final {
    std::array<char, hash_names::kNameLength> name{};
    std::array<std::uint16_t, kSliceCapacity> sliceSets{};
    std::uint32_t nameHash{};
    std::uint16_t mapIndex{};
    std::uint8_t ordinal{};
    std::uint8_t nameLength{};
    std::uint8_t sliceCount{};
};

struct SpawnSet final {
    std::array<char, hash_names::kNameLength> name{};
    std::uint32_t hash{};
    std::uint32_t pointCount{};
    std::uint8_t nameLength{};
    bool offered{};
    bool candidate{};
    bool loaded{};
    bool loadKnown{};
};

struct Details final {
    Summary world{};
    std::array<Bubble, kBubbleCapacity> bubbles{};
    std::array<SpawnSet, kSpawnCapacity> spawnSets{};
    std::size_t bubbleCount{};
    std::size_t spawnSetCount{};
    std::size_t hiddenSpawnSetCount{};
    std::size_t foreignSpawnSetCount{};
    bool spawnCatalogAvailable{};
    bool spawnSetsNarrowed{};
};

[[nodiscard]] std::string_view name_of(const Summary& world) noexcept;
[[nodiscard]] std::string_view stem_of(const Summary& world) noexcept;
[[nodiscard]] std::string_view name_of(const Bubble& bubble) noexcept;
[[nodiscard]] std::string_view name_of(const SpawnSet& spawnSet) noexcept;
[[nodiscard]] std::size_t revision() noexcept;

/** Copies compact summaries for every installed destination in stable name order. */
[[nodiscard]] bool
snapshot(std::span<Summary> output, std::size_t& count, std::size_t& revision) noexcept;

/**
 * Resolves one destination and its launch choices. When `bubble` is nonnegative, spawn sets are
 * narrowed to that map-global bubble and each slice set belonging to the bubble is published.
 */
[[nodiscard]] bool
inspect(std::string_view worldName, std::int16_t bubble, Details& details) noexcept;

} // namespace sunrise::state::build_data::worlds
