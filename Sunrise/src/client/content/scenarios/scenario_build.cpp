#include "scenario_build.h"

#include <array>
#include <cstdio>

#include <Windows.h>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../state/build_data/runtime.h"
#include "../handles/handle_debug_dump.h"
#include "../../memory/current_process_memory.h"
#include "../../diagnostics/module_range.h"
#include "../../targets/game/content.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace packages = middleware::content::packages;

/** Dumps related build-86657 records so the sequence layout has known comparison controls. */
void report_activity_auth_schemas() noexcept {
    if (!targets::game::content::is_resolved()) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=spawner_probe stage=sequence_schema result=targets_unavailable");
        return;
    }
    const handles::Source source{
        reinterpret_cast<std::uintptr_t>(targets::game::content::get().contentHandleTablesSlot),
        nullptr,
        &memory::read_current_process};
    constexpr handles::debug::Window window{512, 4096, 32};
    (void)handles::debug::dump(source, 0x80804F01, "sequence_component", window);
    (void)handles::debug::dump(source, 0x80804F04, "sequence_auth", window);
    (void)handles::debug::dump(source, 0x80804F47, "device_sense", window);
    (void)handles::debug::dump(source, 0x80804F48, "device_auth", window);
    (void)handles::debug::dump(source, 0x808094EE, "engagement_sensor_component", window);
    (void)handles::debug::dump(source, 0x808094F0, "engagement_sensor_sense", window);
    (void)handles::debug::dump(source, 0x808094F1, "engagement_sensor_auth", window);
    (void)handles::debug::dump(source, 0x80808348, "drill_objective_component", window);
    (void)handles::debug::dump(source, 0x80807F04, "drill_objective_sense", window);
    (void)handles::debug::dump(source, 0x80807F0C, "drill_objective_auth", window);
    (void)handles::debug::dump(source, 0x8080953F, "event_hopon_component", window);
    (void)handles::debug::dump(source, 0x8080954A, "event_hopon_sense", window);
    (void)handles::debug::dump(source, 0x8080954B, "event_hopon_auth", window);
    (void)handles::debug::dump(source, 0x80804F3B, "drill_laser_component", window);
    (void)handles::debug::dump(source, 0x80804F3D, "drill_laser_sense", window);
    (void)handles::debug::dump(source, 0x80804F40, "drill_laser_auth", window);
    (void)handles::debug::dump(source, 0x8080952F, "placement_engagement_component", window);
    (void)handles::debug::dump(source, 0x80809531, "placement_engagement_sense", window);
    (void)handles::debug::dump(source, 0x80809532, "placement_engagement_auth", window);
    (void)handles::debug::dump(source, 0x80809A3B, "dropship_component", window);
    (void)handles::debug::dump(source, 0x80807ECC, "dropship_sense", window);
    (void)handles::debug::dump(source, 0x80807EC9, "spawner_auth", window);
    diagnostics::ModuleRange image{};
    if (diagnostics::module_range(GetModuleHandleW(nullptr), image)) {
        constexpr std::array actionProbes{
            handles::debug::SchemaProbe{0x80804F01, "sequence_component"},
            handles::debug::SchemaProbe{0x80804F04, "sequence_auth"},
            handles::debug::SchemaProbe{0x80804F47, "device_sense"},
            handles::debug::SchemaProbe{0x80804F48, "device_auth"},
            handles::debug::SchemaProbe{0x80804F5D, "sequence_entries"},
            handles::debug::SchemaProbe{0x80809C42, "sequence_stamp"},
            handles::debug::SchemaProbe{0x80809291, "sequence_entry_keys"},
            handles::debug::SchemaProbe{0x80802DB5, "sequence_entry_values"},
            handles::debug::SchemaProbe{0x80809A3B, "dropship_component"},
            handles::debug::SchemaProbe{0x80807ECC, "dropship_sense"},
            handles::debug::SchemaProbe{0x80807EC9, "spawner_auth"},
        };
        constexpr std::array lifecycleProbes{
            handles::debug::SchemaProbe{0x808094EE, "engagement_sensor_component"},
            handles::debug::SchemaProbe{0x808094F0, "engagement_sensor_sense"},
            handles::debug::SchemaProbe{0x808094F1, "engagement_sensor_auth"},
            handles::debug::SchemaProbe{0x80808348, "drill_objective_component"},
            handles::debug::SchemaProbe{0x80807F04, "drill_objective_sense"},
            handles::debug::SchemaProbe{0x80807F0C, "drill_objective_auth"},
            handles::debug::SchemaProbe{0x8080953F, "event_hopon_component"},
            handles::debug::SchemaProbe{0x8080954A, "event_hopon_sense"},
            handles::debug::SchemaProbe{0x8080954B, "event_hopon_auth"},
            handles::debug::SchemaProbe{0x80804F3B, "drill_laser_component"},
            handles::debug::SchemaProbe{0x80804F3D, "drill_laser_sense"},
            handles::debug::SchemaProbe{0x80804F40, "drill_laser_auth"},
            handles::debug::SchemaProbe{0x8080952F, "placement_engagement_component"},
            handles::debug::SchemaProbe{0x80809531, "placement_engagement_sense"},
            handles::debug::SchemaProbe{0x80809532, "placement_engagement_auth"},
        };
        static_assert(actionProbes.size() <= 16 && lifecycleProbes.size() <= 16);
        const handles::debug::Range range{image.base, image.end - image.base};
        (void)handles::debug::dump_schema_bindings(source, range, actionProbes, window);
        (void)handles::debug::dump_schema_bindings(source, range, lifecycleProbes, window);
    }
}

/**
 * Reports the pass so a boot that falls back to authored defaults says which step lost the rows.
 * Each count is separate on purpose: a sweep that finds tags, a name match that finds none, and a
 * blob read that drops every row all end with no domain and need different fixes.
 * @param storage Pass storage holding every count.
 * @param kept Rows whose blob read and parsed.
 * @param rostered Rows that published at least one roster group.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage,
            std::size_t kept,
            std::size_t rostered,
            const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=build_data stage=scenarios tags=%zu named=%zu live=%zu kept=%zu "
                      "groups=%zu dropped=%zu rostered=%zu result=%s",
                      storage.liveTagCount,
                      storage.rowCount,
                      storage.liveRowCount,
                      kept,
                      storage.roster.groupCount,
                      storage.roster.unresolvedGroups,
                      rostered,
                      result);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         kept != 0 ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Walks the next batch of rosters and publishes the domain once the walk finishes.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage holding the collected rows and the walk cursor.
 * @return True only when the whole domain is published.
 */
[[nodiscard]] bool walk_rosters(const packages::reader::Source& source,
                                packages::reader::Scratch& scratch,
                                Storage& storage) noexcept {
    const auto rows = std::span(storage.rows).first(storage.keptCount);
    if (!build_rosters(source, scratch, storage.roster, rows)) {
        return false;
    }
    std::size_t rostered = 0;
    for (const layouts::Definition& row : rows) {
        rostered += row.rosterGroupCount != 0 ? 1U : 0U;
    }
    const bool published = state::build_data::publish_scenario_layouts(
        rows, std::span(storage.roster.groups).first(storage.roster.groupCount));
    report(storage, storage.keptCount, rostered, published ? "ok" : "publish");
    if (core::settings::get().server.activation.trostlandSpawnerProbe) {
        report_activity_auth_schemas();
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=spawner_probe stage=package_scan result=%s package=0x0213 descriptors=%zu",
            storage.roster.probeDescriptorCount != 0 ? "found" : "missing",
            storage.roster.probeDescriptorCount);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             storage.roster.probeDescriptorCount != 0 ? core::log::Level::info
                                                                      : core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    storage.roster = {};
    return published;
}

} // namespace

/** Extracts every destination's bubble layout and roster from the installed packages, once. */
bool build(const packages::reader::Source& source, packages::reader::Scratch& scratch) noexcept {
    if (state::build_data::scenario_layouts_ready()) {
        static bool probeReported = false;
        if (!probeReported && core::settings::get().server.activation.trostlandSpawnerProbe
            && targets::game::content::is_resolved()) {
            if (probe_trostland_roster(source, scratch)) {
                report_activity_auth_schemas();
                probeReported = true;
            }
        }
        return true;
    }
    static Storage storage{};
    if (!storage.collected) {
        const char* reason = nullptr;
        if (!collect_rows(source, storage, reason)) {
            report(storage, 0, 0, reason);
            return false;
        }
        storage.collected = true;
        report(storage, 0, 0, "collecting");
        return false;
    }
    if (!storage.compacted) {
        if (!resolve_pending(source, scratch, storage)) {
            return false;
        }
        compact_rows(storage);
        // An empty result is never a finished pass. Every blob read needs the block keys, and
        // those arrive during the boot, so a window that closes first reads nothing. Latching it
        // would publish a domain with no destinations for the whole run. Only the first empty
        // round reports, because the retry runs on every worker slice.
        if (storage.keptCount == 0) {
            if (storage.resolveRounds == 0) {
                report(storage, 0, 0, "empty");
            }
            rearm_resolve(storage);
            return false;
        }
        report(storage, storage.keptCount, 0, "collected");
        storage.compacted = true;
    }
    return walk_rosters(source, scratch, storage);
}

} // namespace sunrise::client::content::scenarios
