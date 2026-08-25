#include "world_catalog.h"

#include <algorithm>

#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../runtime.h"

namespace sunrise::state::build_data::worlds {
namespace {

namespace tables = middleware::content::packages::tables;

[[nodiscard]] Summary summarize(const scenarios::Definition& definition) noexcept {
    Summary result{};
    result.name = definition.name;
    result.mapStem = definition.spawnStem;
    result.scenarioTag = definition.tag;
    result.nameLength = definition.nameLength;
    result.mapStemLength = definition.spawnStemLength;
    result.bubbleCount = definition.bubbleCount;
    result.truncated = definition.truncated != 0;
    return result;
}

[[nodiscard]] std::string_view definition_name(const scenarios::Definition& definition) noexcept {
    return {definition.name.data(), definition.nameLength};
}

[[nodiscard]] std::string_view definition_stem(const scenarios::Definition& definition) noexcept {
    return {definition.spawnStem.data(), definition.spawnStemLength};
}

void resolve_name(std::uint32_t hash,
                  std::array<char, hash_names::kNameLength>& output,
                  std::uint8_t& length) noexcept {
    hash_names::Name resolved{};
    if (!find_hash_name(hash, resolved)) {
        return;
    }
    output = resolved.name;
    length = resolved.nameLength;
}

enum class PackageMatch : std::uint8_t {
    unknown,
    absent,
    present,
};

[[nodiscard]] PackageMatch loads_package(const scenarios::Definition& world,
                                         const spawn_sets::NameHash& spawnSet) noexcept {
    if (spawnSet.inMapPackage != 0) {
        return PackageMatch::present;
    }
    const std::size_t worldPackages =
        (std::min)(static_cast<std::size_t>(world.packageCount), world.packages.size());
    const std::size_t setPackages =
        (std::min)(static_cast<std::size_t>(spawnSet.activityPackageCount),
                   spawnSet.activityPackages.size());
    for (std::size_t candidate = 0; candidate < setPackages; ++candidate) {
        for (std::size_t loaded = 0; loaded < worldPackages; ++loaded) {
            if (spawnSet.activityPackages[candidate] == world.packages[loaded]) {
                return PackageMatch::present;
            }
        }
    }
    return spawnSet.activityPackageOverflow != 0 ? PackageMatch::unknown : PackageMatch::absent;
}

[[nodiscard]] bool bubble_offers(const spawn_sets::NameHash& spawnSet,
                                 std::uint16_t mapIndex) noexcept {
    const std::size_t byte = mapIndex / 8U;
    return byte < spawnSet.bubbleMask.size()
           && (spawnSet.bubbleMask[byte] & static_cast<std::uint8_t>(1U << (mapIndex % 8U))) != 0;
}

void append_bubbles(const scenarios::Definition& world, Details& details) noexcept {
    const std::size_t count =
        (std::min)(static_cast<std::size_t>(world.bubbleCount), world.bubbleStates.size());
    for (std::size_t index = 0; index < count; ++index) {
        Bubble& bubble = details.bubbles[details.bubbleCount++];
        bubble.ordinal = static_cast<std::uint8_t>(index);
        bubble.nameHash = world.bubbleHashes[index];
        bubble.mapIndex = world.bubbleMapIndices[index];
        bubble.sliceCount = (std::min)(world.bubbleStateCounts[index],
                                       static_cast<std::uint8_t>(bubble.sliceSets.size()));
        const std::uint32_t first = tables::region_index(static_cast<std::uint8_t>(index));
        for (std::uint8_t state = 0; state < bubble.sliceCount; ++state) {
            bubble.sliceSets[state] = static_cast<std::uint16_t>(first + state);
        }
        resolve_name(bubble.nameHash, bubble.name, bubble.nameLength);
    }
}

void append_spawn_sets(const scenarios::Definition& world,
                       std::int16_t bubbleOrdinal,
                       Details& details) noexcept {
    const std::string_view stem = definition_stem(world);
    if (stem.empty()) {
        details.spawnCatalogAvailable = true;
        return;
    }
    std::array<spawn_sets::NameHash, kSpawnCapacity> rows{};
    std::size_t count = 0;
    if (!find_spawn_sets(stem, rows, count)) {
        return;
    }
    details.spawnCatalogAvailable = true;
    const bool narrowed =
        bubbleOrdinal >= 0 && static_cast<std::size_t>(bubbleOrdinal) < details.bubbleCount;
    details.spawnSetsNarrowed = narrowed;
    const std::uint16_t mapIndex =
        narrowed ? details.bubbles[static_cast<std::size_t>(bubbleOrdinal)].mapIndex
                 : tables::kAbsentMapBubbleIndex;
    for (std::size_t index = 0; index < count && details.spawnSetCount < kSpawnCapacity; ++index) {
        const spawn_sets::NameHash& source = rows[index];
        const bool offered = !narrowed || bubble_offers(source, mapIndex);
        const bool candidate = !offered && source.unbound != 0;
        if (!offered && !candidate) {
            ++details.hiddenSpawnSetCount;
            continue;
        }
        SpawnSet& output = details.spawnSets[details.spawnSetCount++];
        output.hash = source.value;
        output.pointCount = source.pointCount;
        output.offered = offered;
        output.candidate = candidate;
        const PackageMatch package = loads_package(world, source);
        output.loaded = package == PackageMatch::present;
        output.loadKnown = package != PackageMatch::unknown;
        if (package == PackageMatch::absent) {
            ++details.foreignSpawnSetCount;
        }
        resolve_name(output.hash, output.name, output.nameLength);
    }
}

} // namespace

std::string_view name_of(const Summary& world) noexcept {
    return {world.name.data(), world.nameLength};
}

std::string_view stem_of(const Summary& world) noexcept {
    return {world.mapStem.data(), world.mapStemLength};
}

std::string_view name_of(const Bubble& bubble) noexcept {
    return {bubble.name.data(), bubble.nameLength};
}

std::string_view name_of(const SpawnSet& spawnSet) noexcept {
    return {spawnSet.name.data(), spawnSet.nameLength};
}

std::size_t revision() noexcept {
    return scenario_layout_count();
}

bool snapshot(std::span<Summary> output, std::size_t& count, std::size_t& revision) noexcept {
    count = 0;
    revision = scenario_layout_count();
    if (revision == 0 || output.size() < revision) {
        return false;
    }
    thread_local std::array<scenarios::Definition, kWorldCapacity> definitions{};
    std::size_t definitionCount = 0;
    if (!snapshot_scenario_layouts(definitions, definitionCount)
        || definitionCount > output.size()) {
        return false;
    }
    std::ranges::sort(
        std::span(definitions).first(definitionCount),
        [](const scenarios::Definition& left, const scenarios::Definition& right) noexcept {
            return definition_name(left) < definition_name(right);
        });
    for (std::size_t index = 0; index < definitionCount; ++index) {
        output[index] = summarize(definitions[index]);
    }
    count = definitionCount;
    return true;
}

bool inspect(std::string_view worldName, std::int16_t bubble, Details& details) noexcept {
    details = {};
    scenarios::Definition world{};
    if (worldName.empty() || !find_scenario_layout(worldName, world)) {
        return false;
    }
    details.world = summarize(world);
    append_bubbles(world, details);
    append_spawn_sets(world, bubble, details);
    return true;
}

} // namespace sunrise::state::build_data::worlds
