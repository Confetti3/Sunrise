#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "state/build_data/runtime.h"
#include "state/build_data/worlds/world_catalog.h"

namespace build_data = sunrise::state::build_data;
namespace scenarios = build_data::scenarios;
namespace spawn_sets = build_data::spawn_sets;
namespace worlds = build_data::worlds;

namespace {

std::array<scenarios::Definition, 3> g_worlds{};
std::size_t g_worldCount{};
std::array<build_data::hash_names::Name, 8> g_names{};
std::size_t g_nameCount{};
std::array<spawn_sets::NameHash, 8> g_spawnSets{};
std::size_t g_spawnCount{};

template <std::size_t Capacity>
void assign(std::array<char, Capacity>& output, std::uint8_t& length, std::string_view value) {
    std::copy(value.begin(), value.end(), output.begin());
    length = static_cast<std::uint8_t>(value.size());
}

scenarios::Definition make_world(std::string_view name, std::string_view stem, std::uint32_t tag) {
    scenarios::Definition result{};
    assign(result.name, result.nameLength, name);
    assign(result.spawnStem, result.spawnStemLength, stem);
    result.tag = tag;
    return result;
}

void add_name(std::uint32_t hash, std::string_view name) {
    auto& row = g_names[g_nameCount++];
    row.hash = hash;
    assign(row.name, row.nameLength, name);
}

spawn_sets::NameHash& add_spawn(std::uint32_t hash, std::uint16_t mapIndex) {
    auto& row = g_spawnSets[g_spawnCount++];
    row.value = hash;
    row.pointCount = hash & 0xFFU;
    row.bubbleMask[mapIndex / 8U] |= static_cast<std::uint8_t>(1U << (mapIndex % 8U));
    return row;
}

bool check(bool value) {
    return value;
}

} // namespace

namespace sunrise::state::build_data {

std::size_t scenario_layout_count() noexcept {
    return g_worldCount;
}

bool snapshot_scenario_layouts(std::span<scenarios::Definition> output,
                               std::size_t& count) noexcept {
    count = 0;
    if (output.size() < g_worldCount) {
        return false;
    }
    std::copy_n(g_worlds.begin(), g_worldCount, output.begin());
    count = g_worldCount;
    return true;
}

bool find_scenario_layout(std::string_view name, scenarios::Definition& definition) noexcept {
    definition = {};
    for (std::size_t index = 0; index < g_worldCount; ++index) {
        const auto& candidate = g_worlds[index];
        if (std::string_view(candidate.name.data(), candidate.nameLength) == name) {
            definition = candidate;
            return true;
        }
    }
    return false;
}

bool find_hash_name(std::uint32_t hash, hash_names::Name& name) noexcept {
    name = {};
    for (std::size_t index = 0; index < g_nameCount; ++index) {
        if (g_names[index].hash == hash) {
            name = g_names[index];
            return true;
        }
    }
    return false;
}

bool find_spawn_sets(std::string_view stem,
                     std::span<spawn_sets::NameHash> output,
                     std::size_t& count) noexcept {
    count = 0;
    if (stem != "map_alpha" || output.size() < g_spawnCount) {
        return false;
    }
    std::copy_n(g_spawnSets.begin(), g_spawnCount, output.begin());
    count = g_spawnCount;
    return true;
}

} // namespace sunrise::state::build_data

int main() {
    g_worlds[0] = make_world("zeta", "map_zeta", 3);
    g_worlds[1] = make_world("alpha", "map_alpha", 1);
    g_worlds[2] = make_world("middle", "map_middle", 2);
    g_worldCount = 3;

    std::array<worlds::Summary, worlds::kWorldCapacity> summaries{};
    std::size_t count = 0;
    std::size_t revision = 0;
    if (!check(worlds::snapshot(summaries, count, revision)) || count != 3 || revision != 3
        || worlds::name_of(summaries[0]) != "alpha" || worlds::name_of(summaries[1]) != "middle"
        || worlds::name_of(summaries[2]) != "zeta" || worlds::stem_of(summaries[0]) != "map_alpha"
        || summaries[0].scenarioTag != 1) {
        return 1;
    }
    std::array<worlds::Summary, 2> tooSmall{};
    if (worlds::snapshot(tooSmall, count, revision) || count != 0 || revision != 3) {
        return 2;
    }

    auto& alpha = g_worlds[1];
    alpha.bubbleCount = 2;
    alpha.bubbleHashes[0] = 0x100;
    alpha.bubbleHashes[1] = 0x101;
    alpha.bubbleStateCounts[0] = 1;
    alpha.bubbleStateCounts[1] = 2;
    alpha.bubbleMapIndices[0] = 2;
    alpha.bubbleMapIndices[1] = 9;
    alpha.packageCount = 1;
    alpha.packages[0] = 77;
    add_name(0x100, "arrival");
    add_name(0x101, "interior");

    auto& mapSpawn = add_spawn(0x200, 9);
    mapSpawn.inMapPackage = 1;
    add_name(0x200, "default_spawn");
    auto& activitySpawn = add_spawn(0x201, 9);
    activitySpawn.activityPackageCount = 1;
    activitySpawn.activityPackages[0] = 77;
    auto& foreignSpawn = add_spawn(0x202, 9);
    foreignSpawn.activityPackageCount = 1;
    foreignSpawn.activityPackages[0] = 88;
    auto& hiddenSpawn = add_spawn(0x203, 2);
    hiddenSpawn.inMapPackage = 1;
    auto& candidateSpawn = add_spawn(0x204, 2);
    candidateSpawn.inMapPackage = 1;
    candidateSpawn.unbound = 1;
    auto& unknownSpawn = add_spawn(0x205, 9);
    unknownSpawn.activityPackageOverflow = 1;

    worlds::Details details{};
    if (!worlds::inspect("alpha", 1, details) || details.bubbleCount != 2
        || worlds::name_of(details.bubbles[0]) != "arrival"
        || worlds::name_of(details.bubbles[1]) != "interior" || details.bubbles[1].sliceCount != 2
        || !details.spawnCatalogAvailable || !details.spawnSetsNarrowed
        || details.spawnSetCount != 5 || details.hiddenSpawnSetCount != 1
        || details.foreignSpawnSetCount != 1) {
        return 3;
    }
    const auto find_spawn = [&details](std::uint32_t hash) -> const worlds::SpawnSet* {
        for (std::size_t index = 0; index < details.spawnSetCount; ++index) {
            if (details.spawnSets[index].hash == hash) {
                return &details.spawnSets[index];
            }
        }
        return nullptr;
    };
    const worlds::SpawnSet* map = find_spawn(0x200);
    const worlds::SpawnSet* activity = find_spawn(0x201);
    const worlds::SpawnSet* foreign = find_spawn(0x202);
    const worlds::SpawnSet* candidate = find_spawn(0x204);
    const worlds::SpawnSet* unknown = find_spawn(0x205);
    if (map == nullptr || !map->loaded || worlds::name_of(*map) != "default_spawn"
        || activity == nullptr || !activity->loaded || foreign == nullptr || foreign->loaded
        || candidate == nullptr || !candidate->candidate || candidate->offered || unknown == nullptr
        || unknown->loadKnown || unknown->loaded) {
        return 4;
    }

    if (worlds::inspect("missing", -1, details)) {
        return 5;
    }
    g_worldCount = 0;
    if (worlds::snapshot(summaries, count, revision) || count != 0 || revision != 0) {
        return 6;
    }
    return 0;
}
