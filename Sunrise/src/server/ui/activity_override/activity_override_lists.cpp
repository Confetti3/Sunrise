#include "activity_override_lists.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

#include "../../../middleware/content/packages/tables/spawn_reader.h"

namespace sunrise::server::ui::activity_override {
namespace {

namespace worlds = state::build_data::worlds;
namespace tables = middleware::content::packages::tables;

Lists g_lists{};

[[nodiscard]] std::string_view spawn_name(const worlds::SpawnSet& spawnSet) noexcept {
    const std::string_view resolved = worlds::name_of(spawnSet);
    if (!resolved.empty()) {
        return resolved;
    }
    if (spawnSet.hash == tables::kDefaultSpawnNameHash) {
        return "default";
    }
    return spawnSet.hash == tables::kUnnamedSpawnNameHash ? std::string_view("unnamed")
                                                          : std::string_view();
}

void name_column(std::string_view name, Label& column) noexcept {
    column = {};
    if (!name.empty()) {
        (void)std::snprintf(
            column.data(), column.size(), "  %.*s", static_cast<int>(name.size()), name.data());
    }
}

void assign(std::string_view text, Label& label) noexcept {
    label = {};
    const std::size_t length = (std::min)(text.size(), label.size() - 1);
    std::copy_n(text.begin(), length, label.begin());
}

void clear_slices(Lists& rows) noexcept {
    rows.slices = {};
    rows.sliceValues = {};
    rows.sliceCount = 0;
}

void clear_spawns(Lists& rows) noexcept {
    rows.spawns = {};
    rows.spawnHashes = {};
    rows.spawnSelectable = {};
    rows.spawnCount = 0;
    rows.spawnUnavailable = false;
    rows.spawnNarrowed = false;
    rows.spawnHidden = 0;
    rows.spawnForeign = 0;
}

void clear_destination(Lists& rows) noexcept {
    rows.selected = {};
    rows.authored = {};
    rows.bubbles = {};
    rows.bubbleOrdinals = {};
    rows.bubbleCount = 0;
    clear_slices(rows);
    clear_spawns(rows);
}

void build_bubbles(Lists& rows) noexcept {
    for (std::size_t index = 0;
         index < rows.selected.bubbleCount && rows.bubbleCount < rows.bubbles.size();
         ++index) {
        const worlds::Bubble& bubble = rows.selected.bubbles[index];
        Label name{};
        name_column(worlds::name_of(bubble), name);
        Label label{};
        const int written = std::snprintf(label.data(),
                                          label.size(),
                                          "%2u  0x%08X%s  (%u slice%s)",
                                          static_cast<unsigned>(bubble.ordinal),
                                          bubble.nameHash,
                                          name.data(),
                                          static_cast<unsigned>(bubble.sliceCount),
                                          bubble.sliceCount == 1 ? "" : "s");
        if (written <= 0) {
            continue;
        }
        rows.bubbles[rows.bubbleCount] = label;
        rows.bubbleOrdinals[rows.bubbleCount] = bubble.ordinal;
        ++rows.bubbleCount;
    }
}

void build_slices(Lists& rows, std::uint8_t bubbleOrdinal) noexcept {
    if (bubbleOrdinal >= rows.selected.bubbleCount) {
        return;
    }
    const worlds::Bubble& bubble = rows.selected.bubbles[bubbleOrdinal];
    for (std::uint8_t state = 0; state < bubble.sliceCount && rows.sliceCount < rows.slices.size();
         ++state) {
        Label label{};
        const int written = std::snprintf(label.data(),
                                          label.size(),
                                          "%u  state %u",
                                          bubble.sliceSets[state],
                                          static_cast<unsigned>(state));
        if (written <= 0) {
            continue;
        }
        rows.slices[rows.sliceCount] = label;
        rows.sliceValues[rows.sliceCount] = bubble.sliceSets[state];
        ++rows.sliceCount;
    }
}

void build_spawns(Lists& rows) noexcept {
    rows.spawnUnavailable = !rows.selected.spawnCatalogAvailable;
    rows.spawnNarrowed = rows.selected.spawnSetsNarrowed;
    rows.spawnHidden = rows.selected.hiddenSpawnSetCount;
    rows.spawnForeign = rows.selected.foreignSpawnSetCount;
    for (std::size_t index = 0;
         index < rows.selected.spawnSetCount && rows.spawnCount < rows.spawns.size();
         ++index) {
        const worlds::SpawnSet& spawnSet = rows.selected.spawnSets[index];
        Label name{};
        name_column(spawn_name(spawnSet), name);
        Label label{};
        const int written = std::snprintf(label.data(),
                                          label.size(),
                                          "0x%08X%s%s%s",
                                          spawnSet.hash,
                                          name.data(),
                                          spawnSet.candidate ? "  (candidate)" : "",
                                          !spawnSet.loadKnown ? "  (package unknown)"
                                          : spawnSet.loaded   ? ""
                                                              : "  (not loaded)");
        if (written <= 0) {
            continue;
        }
        rows.spawns[rows.spawnCount] = label;
        rows.spawnHashes[rows.spawnCount] = spawnSet.hash;
        rows.spawnSelectable[rows.spawnCount] = !spawnSet.loadKnown || spawnSet.loaded;
        ++rows.spawnCount;
    }
}

} // namespace

Lists& lists() noexcept {
    return g_lists;
}

void refresh_activities(Lists& rows) noexcept {
    const std::size_t published = worlds::revision();
    if (published == rows.activityRevision) {
        return;
    }
    if (published == 0) {
        rows.activities = {};
        rows.worlds = {};
        rows.activityCount = 0;
        rows.activityRevision = 0;
        clear_destination(rows);
        return;
    }
    std::array<worlds::Summary, worlds::kWorldCapacity> summaries{};
    std::size_t count = 0;
    std::size_t catalogRevision = 0;
    if (!worlds::snapshot(summaries, count, catalogRevision)) {
        return;
    }
    rows.activities = {};
    rows.worlds = {};
    rows.activityCount = 0;
    for (const worlds::Summary& summary : std::span(summaries).first(count)) {
        enrichment::Summary authored{};
        enrichment::resolve(summary, authored);
        Label label{};
        const std::string_view activityName(authored.activityName.data(),
                                            authored.activityNameLength);
        if (activityName.empty()) {
            assign(worlds::name_of(summary), label);
        } else {
            (void)std::snprintf(label.data(),
                                label.size(),
                                "%.*s  ·  %.*s",
                                static_cast<int>(worlds::name_of(summary).size()),
                                worlds::name_of(summary).data(),
                                static_cast<int>(activityName.size()),
                                activityName.data());
        }
        rows.activities[rows.activityCount] = label;
        rows.worlds[rows.activityCount] = summary;
        ++rows.activityCount;
    }
    rows.activityRevision = catalogRevision;
}

void refresh_destination(Lists& rows, std::string_view name) noexcept {
    clear_destination(rows);
    worlds::Details details{};
    if (!worlds::inspect(name, -1, details)) {
        return;
    }
    rows.selected = details;
    enrichment::resolve(rows.selected.world, rows.authored);
    build_bubbles(rows);
    build_spawns(rows);
}

void refresh_bubble(Lists& rows, std::uint8_t bubble) noexcept {
    const std::string_view worldName = worlds::name_of(rows.selected.world);
    if (worldName.empty()) {
        return;
    }
    worlds::Details details{};
    if (!worlds::inspect(worldName, bubble, details)) {
        return;
    }
    clear_slices(rows);
    clear_spawns(rows);
    rows.selected = details;
    enrichment::resolve(rows.selected.world, rows.authored);
    build_slices(rows, bubble);
    build_spawns(rows);
}

} // namespace sunrise::server::ui::activity_override
