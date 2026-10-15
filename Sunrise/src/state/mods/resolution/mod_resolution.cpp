#include "mod_resolution.h"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include "../manifest/mod_version.h"
#include "../problem/mod_problem.h"

namespace sunrise::state::mods::resolution {
namespace {

/** Names no record. */
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
/** Separator written between ids in a cycle message. */
constexpr std::string_view kCycleArrow = " -> ";

/** One parsed id and the record that holds it. */
struct IdSlot final {
    std::string_view id;
    std::size_t record{};
};

/** Parsed ids sorted for lookup. Repeated ids sit next to each other. */
struct Index final {
    std::array<IdSlot, kModCapacity> slots{};
    std::size_t count{};

    /** @return The first record holding `id`, or kNone. */
    [[nodiscard]] std::size_t find(std::string_view id) const noexcept {
        const auto occupied = std::span(slots).first(count);
        const auto found = std::lower_bound(
            occupied.begin(), occupied.end(), id, [](const IdSlot& slot, auto key) {
                return slot.id < key;
            });
        return found != occupied.end() && found->id == id ? found->record : kNone;
    }
};

/** @return True while a parsed mod has no refusal. */
[[nodiscard]] bool loadable(const Record& record) noexcept {
    return record.manifestRead && record.status == Status::loaded;
}

/** @return The id text of a parsed record, null-terminated by its spare byte. */
[[nodiscard]] const char* id_text(const Record& record) noexcept {
    return record.manifest.id.bytes.data();
}

/** Builds the sorted id index over every parsed manifest. */
[[nodiscard]] Index build_index(std::span<const Record> records) noexcept {
    Index index{};
    for (std::size_t record = 0; record < records.size(); ++record) {
        if (records[record].manifestRead) {
            index.slots[index.count++] = IdSlot{records[record].manifest.id.view(), record};
        }
    }
    std::sort(index.slots.begin(),
              index.slots.begin() + static_cast<std::ptrdiff_t>(index.count),
              [](const IdSlot& first, const IdSlot& second) { return first.id < second.id; });
    return index;
}

/** Refuses every folder that shares its id with another folder. */
void refuse_duplicates(std::span<Record> records, const Index& index) noexcept {
    for (std::size_t slot = 0; slot + 1 < index.count; ++slot) {
        const IdSlot& first = index.slots[slot];
        const IdSlot& second = index.slots[slot + 1];
        if (first.id != second.id) {
            continue;
        }
        problem::refuse(records[first.record],
                        Reason::duplicateId,
                        "Id %s is also used by folder %s.",
                        id_text(records[first.record]),
                        records[second.record].folder.bytes.data());
        problem::refuse(records[second.record],
                        Reason::duplicateId,
                        "Id %s is also used by folder %s.",
                        id_text(records[second.record]),
                        records[first.record].folder.bytes.data());
    }
}

/** @return `1.2.0 or newer`, or `1.2.0 to 1.9.0`. */
[[nodiscard]] Text<kProblemCapacity> range_text(const Dependency& dependency) noexcept {
    Text<kProblemCapacity> output{};
    const auto minimum = version::text(dependency.minimum);
    const auto maximum = version::text(dependency.maximum);
    const int written =
        dependency.hasMaximum
            ? std::snprintf(output.bytes.data(),
                            output.bytes.size(),
                            "%s to %s",
                            minimum.data(),
                            maximum.data())
            : std::snprintf(
                  output.bytes.data(), output.bytes.size(), "%s or newer", minimum.data());
    output.length = written > 0 ? static_cast<std::size_t>(written) : 0;
    return output;
}

/**
 * Refuses the first dependency one loadable mod cannot have.
 * @return True when the mod was refused.
 */
[[nodiscard]] bool
check_mod_dependencies(std::span<Record> records, const Index& index, Record& record) noexcept {
    for (std::size_t entry = 0; entry < record.manifest.dependencyCount; ++entry) {
        const Dependency& dependency = record.manifest.dependencies[entry];
        const char* const wanted = dependency.id.bytes.data();
        const std::size_t found = index.find(dependency.id.view());
        if (found == kNone) {
            problem::refuse(record,
                            Reason::missingDependency,
                            "Needs %s %s, which is not installed.",
                            wanted,
                            range_text(dependency).bytes.data());
            return true;
        }
        const Record& target = records[found];
        if (!version::satisfies(dependency, target.manifest.version)) {
            problem::refuse(record,
                            Reason::dependencyVersion,
                            "Needs %s %s; version %s is installed.",
                            wanted,
                            range_text(dependency).bytes.data(),
                            version::text(target.manifest.version).data());
            return true;
        }
        if (!loadable(target)) {
            problem::refuse(
                record, Reason::dependencyNotLoaded, "Needs %s, which did not load.", wanted);
            return true;
        }
    }
    return false;
}

/** Refuses dependents of refused mods until no refusal changes the set. */
void propagate(std::span<Record> records, const Index& index) noexcept {
    bool changed = true;
    while (changed) {
        changed = false;
        for (Record& record : records) {
            if (loadable(record) && check_mod_dependencies(records, index, record)) {
                changed = true;
            }
        }
    }
}

/**
 * Refuses each mod that declares an installed mod incompatible.
 * The set is taken before the pass, so the refusal does not depend on folder order.
 */
void check_incompatible(std::span<Record> records, const Index& index) noexcept {
    std::array<bool, kModCapacity> before{};
    for (std::size_t record = 0; record < records.size(); ++record) {
        before[record] = loadable(records[record]);
    }
    for (std::size_t record = 0; record < records.size(); ++record) {
        if (!before[record]) {
            continue;
        }
        const Manifest& manifest = records[record].manifest;
        for (std::size_t entry = 0; entry < manifest.incompatibleCount; ++entry) {
            const std::size_t found = index.find(manifest.incompatible[entry].view());
            if (found != kNone && before[found]) {
                problem::refuse(records[record],
                                Reason::incompatible,
                                "Incompatible with %s, which is installed. Remove one of them.",
                                manifest.incompatible[entry].bytes.data());
                break;
            }
        }
    }
}

/** @return True when every dependency of the mod is already placed. */
[[nodiscard]] bool ready(const Record& record,
                         const Index& index,
                         const std::array<bool, kModCapacity>& placed) noexcept {
    for (std::size_t entry = 0; entry < record.manifest.dependencyCount; ++entry) {
        const std::size_t found = index.find(record.manifest.dependencies[entry].id.view());
        if (found == kNone || !placed[found]) {
            return false;
        }
    }
    return true;
}

/**
 * Places loadable mods whose dependencies are placed, lowest id first.
 * Mods left unplaced sit on or behind a dependency cycle.
 * @return The number of placed mods.
 */
[[nodiscard]] std::size_t place(std::span<const Record> records,
                                const Index& index,
                                std::array<bool, kModCapacity>& placed,
                                Order& order) noexcept {
    std::size_t count = 0;
    for (;;) {
        std::size_t next = kNone;
        for (std::size_t record = 0; record < records.size(); ++record) {
            if (!loadable(records[record]) || placed[record]
                || !ready(records[record], index, placed)) {
                continue;
            }
            if (next == kNone
                || records[record].manifest.id.view() < records[next].manifest.id.view()) {
                next = record;
            }
        }
        if (next == kNone) {
            return count;
        }
        placed[next] = true;
        order[count++] = next;
    }
}

/** @return The first dependency of `record` that is loadable and not placed, or kNone. */
[[nodiscard]] std::size_t unplaced_dependency(std::span<const Record> records,
                                              const Index& index,
                                              const std::array<bool, kModCapacity>& placed,
                                              const Record& record) noexcept {
    for (std::size_t entry = 0; entry < record.manifest.dependencyCount; ++entry) {
        const std::size_t found = index.find(record.manifest.dependencies[entry].id.view());
        if (found != kNone && loadable(records[found]) && !placed[found]) {
            return found;
        }
    }
    return kNone;
}

/**
 * Follows unplaced dependencies from one mod until an id repeats, then refuses that cycle.
 * Mods that only lead into the cycle are left for dependency propagation.
 */
void refuse_cycle_from(std::span<Record> records,
                       const Index& index,
                       const std::array<bool, kModCapacity>& placed,
                       std::size_t start) noexcept {
    std::array<std::size_t, kModCapacity> path{};
    std::array<std::size_t, kModCapacity> position{};
    position.fill(kNone);
    std::size_t length = 0;
    std::size_t current = start;
    while (current != kNone && position[current] == kNone) {
        position[current] = length;
        path[length++] = current;
        current = unplaced_dependency(records, index, placed, records[current]);
    }
    if (current == kNone) {
        return;
    }
    Text<kProblemCapacity> cycle{};
    auto append = [&cycle](std::string_view part) {
        const std::size_t room = cycle.bytes.size() - 1 - cycle.length;
        const std::size_t copied = (std::min)(room, part.size());
        std::copy_n(part.data(), copied, cycle.bytes.data() + cycle.length);
        cycle.length += copied;
    };
    for (std::size_t step = position[current]; step < length; ++step) {
        append(records[path[step]].manifest.id.view());
        append(kCycleArrow);
    }
    append(records[current].manifest.id.view());
    for (std::size_t step = position[current]; step < length; ++step) {
        problem::refuse(records[path[step]],
                        Reason::dependencyCycle,
                        "Circular dependency: %s.",
                        cycle.bytes.data());
    }
}

} // namespace

/** Decides which mods load and in what order. */
std::size_t resolve(std::span<Record> records, Order& order) noexcept {
    order = {};
    if (records.size() > kModCapacity) {
        return 0;
    }
    const Index index = build_index(records);
    refuse_duplicates(records, index);
    propagate(records, index);
    check_incompatible(records, index);
    propagate(records, index);

    std::array<bool, kModCapacity> placed{};
    const std::size_t count = place(records, index, placed, order);
    for (std::size_t record = 0; record < records.size(); ++record) {
        if (loadable(records[record]) && !placed[record]) {
            refuse_cycle_from(records, index, placed, record);
        }
    }
    // Mods behind a cycle now name the refused cycle member they needed.
    propagate(records, index);
    return count;
}

} // namespace sunrise::state::mods::resolution
