#include <Windows.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/config_bake.h"
#include "../../content_manifest/fingerprint/content_manifest_fingerprint.h"
#include "../discovery/mod_directory_listing.h"
#include "../problem/mod_problem.h"
#include "internal.h"

namespace sunrise::state::mods::packages::internal {
namespace {

namespace bake = middleware::content::packages::config_bake;
namespace layout = middleware::content::packages::reader::layout;

/** One entry of one package. */
using EntryKey = std::pair<std::uint16_t, std::uint32_t>;

/**
 * Separates merge keys from other Sunrise SHA-256 uses. The last byte is the key version;
 * bump it when the merge key inputs change.
 */
constexpr std::string_view kMergeDomain{"SunriseModMerge\0\3", 17};
/** Separates the stem from the key in a merged file name. */
constexpr std::wstring_view kKeySeparator = L".";
/** Ends every reason a mod's merge data was ignored; one ignored file ignores all of it. */
constexpr std::string_view kAllIgnored = ", so all of its merge data was ignored";
/** Why a package file of a mod with merge data has none it can use. */
constexpr std::string_view kNoMergeData = "the file has no usable merge data";
/** Why merge data beside an older generation of a new package cannot be used. */
constexpr std::string_view kOlderGeneration = "it describes an older generation";

/** One package file whose entries the table merge reads. */
struct Source final {
    std::size_t record{};
    const std::wstring* path{};
    const table_merge::Sidecar* sidecar{};
    std::uint16_t packageId{};
    /** Entries to read, sorted and unique before the read. */
    std::vector<std::uint32_t> indices;
    /** One copy per index, read from this file alone. */
    std::vector<std::vector<std::byte>> bytes;
    bake::Status read{bake::Status::unreadable};
};

/** One entry the merge takes, with its sources in load order. */
struct Planned final {
    std::uint16_t packageId{};
    std::uint32_t entryIndex{};
    std::vector<std::size_t> sources;
};

/** The first package file whose merge data a mod cannot use, and why. */
struct Unusable final {
    std::uint16_t packageId{};
    std::string reason;
};

/** Records whose merge data is ignored whole, by record. */
using UnusableRecords = std::map<std::size_t, Unusable>;

/** @return `reason` with the note that the mod's other merge data was ignored too. */
[[nodiscard]] std::string all_ignored(std::string_view reason) {
    std::string text(reason);
    text += kAllIgnored;
    return text;
}

/** @return True when `added` is the newest generation of its package, the one served. */
[[nodiscard]] bool newest(const Work& work, const AddedFile& added) noexcept {
    const Base& base = work.bases[added.packageId];
    return base.owner == added.record && base.patch == added.patch;
}

/**
 * Finds mods whose merge data covers only some of their package files. Their rows may move,
 * and a file with no merge data could not follow them.
 * @return Per such record, the first file that breaks the rule.
 */
[[nodiscard]] UnusableRecords partial_records(const Work& work) {
    std::set<std::size_t> described;
    for (const Claim& claim : work.claims) {
        if (claim.sidecar != kNoSidecar) {
            described.insert(claim.record);
        }
    }
    for (const AddedFile& added : work.added) {
        if (added.sidecar != kNoSidecar && newest(work, added)) {
            described.insert(added.record);
        }
    }
    UnusableRecords unusable;
    for (const Claim& claim : work.claims) {
        if (claim.sidecar == kNoSidecar && described.contains(claim.record)) {
            unusable.try_emplace(claim.record,
                                 Unusable{claim.file.row.packageId, all_ignored(kNoMergeData)});
        }
    }
    for (const AddedFile& added : work.added) {
        const bool served = newest(work, added);
        if (served && added.sidecar == kNoSidecar && described.contains(added.record)) {
            unusable.try_emplace(added.record,
                                 Unusable{added.packageId, all_ignored(kNoMergeData)});
        } else if (!served && added.sidecar != kNoSidecar) {
            unusable.try_emplace(added.record,
                                 Unusable{added.packageId, all_ignored(kOlderGeneration)});
        }
    }
    return unusable;
}

/** @return True when any claim on `packageId` changes `entry`. */
[[nodiscard]] bool
claimed(const Work& work, std::uint16_t packageId, std::uint32_t entry) noexcept {
    return std::any_of(work.claims.begin(), work.claims.end(), [&](const Claim& claim) {
        return claim.file.row.packageId == packageId && claim.changed.test(entry);
    });
}

/**
 * Plans one claim group: entries several claims change and one sidecar describes, and entries
 * one claim changes whose sidecar entry holds sites.
 * @param first First claim of the group; `sources[i]` is claim `i`.
 */
void plan_group(const Work& work,
                std::size_t first,
                std::size_t last,
                std::span<const Source> sources,
                std::vector<Planned>& planned) {
    const std::uint16_t packageId = work.claims[first].file.row.packageId;
    for (std::uint32_t entry = 0; entry < merge::kEntryLimit; ++entry) {
        Planned item{packageId, entry, {}};
        bool described = false;
        bool sites = false;
        for (std::size_t index = first; index < last; ++index) {
            if (!work.claims[index].changed.test(entry)) {
                continue;
            }
            item.sources.push_back(index);
            const table_merge::Sidecar* sidecar = sources[index].sidecar;
            const table_merge::Entry* spec = sidecar == nullptr ? nullptr : sidecar->entry(entry);
            described = described || spec != nullptr;
            sites = sites || (spec != nullptr && !spec->sites.empty());
        }
        if ((item.sources.size() > 1 && described) || (item.sources.size() == 1 && sites)) {
            planned.push_back(std::move(item));
        }
    }
}

/**
 * Plans the entries of each newest new package file whose merge data holds sites.
 * Entries a claim changes are served from the claim, so they are left out.
 */
void plan_added(const Work& work, std::vector<Source>& sources, std::vector<Planned>& planned) {
    for (const AddedFile& added : work.added) {
        if (added.sidecar == kNoSidecar || !newest(work, added)) {
            continue;
        }
        const table_merge::Sidecar& sidecar = work.sidecars[added.sidecar];
        sources.push_back({added.record, &added.path, &sidecar, added.packageId});
        for (const table_merge::Entry& spec : sidecar.entries()) {
            const std::uint32_t entry = spec.header.entryIndex;
            if (!spec.sites.empty() && !claimed(work, added.packageId, entry)) {
                planned.push_back({added.packageId, entry, {sources.size() - 1}});
            }
        }
    }
}

/**
 * Reads every planned entry from each source's own file. Merge data that names entries the file
 * does not hold in plaintext cannot be used, so its mod joins `unusable`.
 */
void read_sources(std::span<const Planned> planned,
                  std::span<Source> sources,
                  UnusableRecords& unusable) {
    for (const Planned& item : planned) {
        for (const std::size_t source : item.sources) {
            sources[source].indices.push_back(item.entryIndex);
        }
    }
    for (Source& source : sources) {
        if (source.indices.empty()) {
            continue;
        }
        std::sort(source.indices.begin(), source.indices.end());
        source.indices.erase(std::unique(source.indices.begin(), source.indices.end()),
                             source.indices.end());
        source.read = bake::read_entries(source.path->c_str(), source.indices, source.bytes);
        if (source.read != bake::Status::ok && source.sidecar != nullptr) {
            unusable.try_emplace(
                source.record,
                Unusable{source.packageId, all_ignored(bake::status_text(source.read))});
        }
    }
}

/** Ignores every sidecar of each unusable record and warns it once. */
void ignore_unusable(Work& work, const UnusableRecords& unusable, std::span<Source> sources) {
    for (Source& source : sources) {
        if (unusable.contains(source.record)) {
            source.sidecar = nullptr;
        }
    }
    for (const auto& [record, item] : unusable) {
        warn_ignored(work.records[record], item.packageId, item.reason.c_str());
    }
}

/**
 * @return The merge inputs of every planned entry. An unread source gives an empty copy with
 * no merge data, so the entry cannot merge and is served later-wins.
 */
[[nodiscard]] std::vector<table_merge::EntryInputs> inputs_of(std::span<const Planned> planned,
                                                              std::span<const Source> sources) {
    std::vector<table_merge::EntryInputs> entries;
    for (const Planned& item : planned) {
        table_merge::EntryInputs& entry =
            entries.emplace_back(table_merge::EntryInputs{item.packageId, item.entryIndex, {}});
        for (const std::size_t index : item.sources) {
            const Source& source = sources[index];
            if (source.read != bake::Status::ok) {
                entry.inputs.push_back({source.record, nullptr, {}});
                continue;
            }
            const auto found =
                std::lower_bound(source.indices.begin(), source.indices.end(), item.entryIndex);
            const auto copy = static_cast<std::size_t>(found - source.indices.begin());
            entry.inputs.push_back({source.record, source.sidecar, source.bytes[copy]});
        }
    }
    return entries;
}

/** Marks one entry as merged, so the overlap warning skips it. */
void mark_merged(Work& work, std::uint16_t packageId, std::uint32_t entry) {
    auto found = std::find_if(work.merged.begin(), work.merged.end(), [&](const auto& item) {
        return item.packageId == packageId;
    });
    if (found == work.merged.end()) {
        found = work.merged.insert(work.merged.end(), MergedEntries{packageId});
    }
    found->entries.set(entry);
}

/** Raises the merge's warnings, writes its log lines and keeps the merged entries. */
void report(Work& work,
            std::span<const table_merge::EntryInputs> entries,
            const table_merge::Outcome& outcome) {
    for (const table_merge::Ignored& item : outcome.ignored) {
        warn_ignored(work.records[item.mod],
                     item.packageId,
                     all_ignored(table_merge::status_text(item.reason)).c_str());
    }
    for (const table_merge::Conflict& item : outcome.conflicts) {
        problem::warn(
            work.records[item.laterMod],
            Reason::assetOverride,
            "Changes %u bytes of entry 0x%08X that %s also changes; its values were used.",
            item.bytes,
            layout::tag_of(item.packageId, item.entryIndex),
            work.records[item.earlierMod].manifest.id.bytes.data());
    }
    for (const table_merge::Dropped& item : outcome.dropped) {
        problem::warn(work.records[item.mod],
                      Reason::mergeFailed,
                      "Its rows added to entry 0x%08X were not merged: %s. Its references to them "
                      "may point at other rows.",
                      item.tableTag,
                      table_merge::status_text(outcome.rows));
    }
    std::map<EntryKey, table_merge::Status> fallbacks;
    for (const table_merge::Fallback& item : outcome.fallbacks) {
        fallbacks.try_emplace({item.packageId, item.entryIndex}, item.reason);
    }
    for (const table_merge::EntryInputs& entry : entries) {
        const auto fallback = fallbacks.find({entry.packageId, entry.entryIndex});
        const bool merged = fallback == fallbacks.end();
        if (merged && entry.inputs.size() > 1) {
            mark_merged(work, entry.packageId, entry.entryIndex);
        }
        core::log::writef(
            core::log::Channel::state,
            merged ? core::log::Level::info : core::log::Level::warn,
            "ev=mods stage=table_merge package=0x%04X entry=%u mods=%zu result=%s "
            "reason=%s",
            entry.packageId,
            entry.entryIndex,
            entry.inputs.size(),
            merged ? "ok" : "fallback",
            table_merge::status_name(merged ? table_merge::Status::ok : fallback->second));
    }
    const bool rows = outcome.rows == table_merge::Status::ok;
    core::log::writef(core::log::Channel::state,
                      rows ? core::log::Level::info : core::log::Level::warn,
                      "ev=mods stage=table_plan result=%s reason=%s table=0x%08X entries=%zu "
                      "replaced=%zu",
                      rows ? "ok" : "off",
                      table_merge::status_name(outcome.rows),
                      outcome.rowsTable,
                      entries.size(),
                      outcome.replacements.size());
}

/** @return The records with an input to any replaced entry, sorted and unique. */
[[nodiscard]] std::vector<std::size_t>
replacement_mods(std::span<const table_merge::EntryInputs> entries,
                 std::span<const table_merge::Replacement> replacements) {
    std::set<EntryKey> replaced;
    for (const table_merge::Replacement& item : replacements) {
        replaced.insert({item.packageId, item.entryIndex});
    }
    std::vector<std::size_t> mods;
    for (const table_merge::EntryInputs& entry : entries) {
        if (!replaced.contains({entry.packageId, entry.entryIndex})) {
            continue;
        }
        for (const table_merge::Input& input : entry.inputs) {
            mods.push_back(input.mod);
        }
    }
    std::sort(mods.begin(), mods.end());
    mods.erase(std::unique(mods.begin(), mods.end()), mods.end());
    return mods;
}

/** @return The end of the claim group that starts at `first`. */
[[nodiscard]] std::size_t group_end(const Work& work, std::size_t first) noexcept {
    std::size_t last = first + 1;
    while (last < work.claims.size()
           && work.claims[last].file.row.packageId == work.claims[first].file.row.packageId) {
        ++last;
    }
    return last;
}

/**
 * Merges every entry mods share and shifts the row numbers they added, once per boot.
 * Claims must be sorted by package id, each group in load order.
 */
void plan_tables(Work& work) {
    std::vector<Source> sources;
    sources.reserve(work.claims.size() + work.added.size());
    for (const Claim& claim : work.claims) {
        sources.push_back({claim.record,
                           &claim.path,
                           claim.sidecar == kNoSidecar ? nullptr : &work.sidecars[claim.sidecar],
                           claim.file.row.packageId});
    }
    std::vector<Planned> planned;
    for (std::size_t first = 0; first < work.claims.size();) {
        const std::size_t last = group_end(work, first);
        plan_group(work, first, last, sources, planned);
        first = last;
    }
    plan_added(work, sources, planned);
    UnusableRecords unusable = partial_records(work);
    read_sources(planned, sources, unusable);
    ignore_unusable(work, unusable, sources);
    if (planned.empty()) {
        return;
    }
    const std::vector<table_merge::EntryInputs> entries = inputs_of(planned, sources);
    table_merge::Outcome outcome = table_merge::merge(entries);
    report(work, entries, outcome);
    work.replacementMods = replacement_mods(entries, outcome.replacements);
    work.replacements = std::move(outcome.replacements);
}

/**
 * Names the merged file for one base and its patches in load order.
 * @return `<stem>.<key>.pkg`, or empty when a file cannot be hashed.
 */
[[nodiscard]] std::wstring merged_leaf(const Work& work,
                                       const Base& base,
                                       std::span<const Claim> group,
                                       SourceInputs& inputs) {
    content_manifest::fingerprint::Hasher hasher;
    bool hashed = hasher.update(std::as_bytes(std::span(kMergeDomain)))
                  && inputs.hash(hasher, base_path(work, base));
    for (const Claim& claim : group) {
        hashed = hashed && inputs.hash(hasher, claim.path);
    }
    content_manifest::Fingerprint key{};
    if (!hashed || !hasher.finish(key)) {
        return {};
    }
    return cache_leaf_name({group.front().file.row.name.data(), group.front().file.row.nameLength},
                           kKeySeparator,
                           key);
}

/** Warns each later mod of one claim group about entries an earlier mod also changes. */
void warn_group(Work& work, std::span<const Claim> group) {
    const std::uint16_t packageId = group.front().file.row.packageId;
    const auto merged = std::find_if(work.merged.begin(), work.merged.end(), [&](const auto& item) {
        return item.packageId == packageId;
    });
    std::bitset<merge::kEntryLimit> open;
    open.set();
    if (merged != work.merged.end()) {
        open &= ~merged->entries;
    }
    for (std::size_t later = 1; later < group.size(); ++later) {
        for (std::size_t earlier = 0; earlier < later; ++earlier) {
            const std::size_t shared =
                (group[earlier].changed & group[later].changed & open).count();
            if (shared == 0) {
                continue;
            }
            problem::warn(work.records[group[later].record],
                          Reason::assetOverride,
                          "Replaces %zu assets in package 0x%04X that %s also changes.",
                          shared,
                          group[later].file.row.packageId,
                          work.records[group[earlier].record].manifest.id.bytes.data());
        }
    }
}

/** @return True when `path` names an existing file. */
[[nodiscard]] bool file_exists(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(core::path::extended(path.c_str()).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/**
 * Merges one group of patches on the same package id, or serves the first patch alone.
 * @return The merged file leaf, or empty when the group was served without a merge.
 */
std::wstring
serve_group(Work& work, std::span<const Claim> group, std::vector<std::wstring>& keep) {
    const std::uint16_t packageId = group.front().file.row.packageId;
    const Base& base = work.bases[packageId];
    SourceInputs inputs;
    std::wstring leaf = merged_leaf(work, base, group, inputs);
    merge::Status status = merge::Status::writeFailed;
    OverlayFile merged = group.front().file;
    // The merged header is the base header, so the row carries the base build signature.
    merged.row.buildSignature = base.newest.buildSignature;
    if (!leaf.empty() && ensure_merge_directory(work)) {
        const std::wstring output = join(work.paths.mergeDirectory, leaf);
        std::vector<const wchar_t*> sources;
        for (const Claim& claim : group) {
            sources.push_back(claim.path.c_str());
        }
        status = file_exists(output)
                         && merge::matches(base_path(work, base).c_str(), sources, output.c_str())
                     ? merge::Status::ok
                     : merge::write(base_path(work, base).c_str(), sources, output.c_str());
        if (status == merge::Status::ok && !client_path(work, output, merged)) {
            status = merge::Status::writeFailed;
        }
    }
    if (status == merge::Status::ok) {
        work.files.push_back(merged);
        return leaf;
    }
    // A failed rebuild must leave the prior cache available for diagnosis and retry.
    if (!leaf.empty() && file_exists(join(work.paths.mergeDirectory, leaf))) {
        keep.push_back(leaf);
    }
    const auto required =
        std::find_if(work.bindings.begin(), work.bindings.end(), [packageId](const Bound& binding) {
            return package_id(binding.site.tag) == packageId;
        });
    if (required != work.bindings.end()) {
        problem::refuse(work.records[required->record],
                        Reason::mergeFailed,
                        "Package 0x%04X has required config bindings; merge failed: %s.",
                        packageId,
                        merge::status_text(status));
        core::log::writef(
            core::log::Channel::state,
            core::log::Level::error,
            "ev=mods stage=merge package=0x%04X result=fail reason=%s config=required",
            packageId,
            merge::status_name(status));
        work.failed = required->record;
        work.complete = false;
        return {};
    }
    // Merged rows assume every patch is served, so none is served in any package.
    drop_replacements(work, packageId, merge::status_text(status));
    std::erase_if(work.merged,
                  [packageId](const MergedEntries& item) { return item.packageId == packageId; });
    // The first patch still loads alone; each later mod says its changes were dropped.
    for (std::size_t index = 1; index < group.size(); ++index) {
        problem::warn(work.records[group[index].record],
                      Reason::mergeFailed,
                      "Its changes to package 0x%04X were not applied: %s.",
                      packageId,
                      merge::status_text(status));
    }
    work.files.push_back(group.front().file);
    return {};
}

} // namespace

/** Creates the merge directory and its parent when they are missing. */
bool ensure_merge_directory(const Work& work) {
    const std::wstring path = core::path::extended(std::wstring(work.paths.mergeDirectory).c_str());
    const std::size_t parent = path.find_last_of(L'\\');
    if (parent != std::wstring::npos) {
        (void)CreateDirectoryW(path.substr(0, parent).c_str(), nullptr);
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE
           || GetLastError() == ERROR_ALREADY_EXISTS;
}

/** Merges shared tables, then serves each patched package id directly or merged. */
std::size_t serve_claims(Work& work) {
    // A stable sort keeps each package group in load order.
    std::stable_sort(
        work.claims.begin(), work.claims.end(), [](const Claim& first, const Claim& second) {
            return first.file.row.packageId < second.file.row.packageId;
        });
    plan_tables(work);
    auto& keep = work.cacheLeaves;
    std::size_t merged = 0;
    std::size_t first = 0;
    while (first < work.claims.size()) {
        const std::size_t last = group_end(work, first);
        const std::span<const Claim> group(work.claims.data() + first, last - first);
        if (group.size() == 1) {
            work.files.push_back(group.front().file);
        } else if (std::wstring leaf = serve_group(work, group, keep); !leaf.empty()) {
            keep.push_back(std::move(leaf));
            ++merged;
        }
        if (!work.complete) {
            return merged;
        }
        first = last;
    }
    return merged;
}

/** Clears every replacement and merged mark, and warns each mod that had rows in them. */
void drop_replacements(Work& work, std::uint16_t packageId, const char* reason) {
    if (work.replacements.empty()) {
        return;
    }
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=mods stage=table_merge result=dropped package=0x%04X replaced=%zu",
                      packageId,
                      work.replacements.size());
    for (const std::size_t mod : work.replacementMods) {
        problem::warn(work.records[mod],
                      Reason::mergeFailed,
                      "Its rows were not merged: package 0x%04X failed: %s. Its references to "
                      "them may point at other rows.",
                      packageId,
                      reason);
    }
    work.replacements.clear();
    work.replacementMods.clear();
    work.merged.clear();
}

/** Runs after the last drop, so entries served later-wins always raise the warning. */
void warn_overlaps(Work& work) {
    for (std::size_t first = 0; first < work.claims.size();) {
        const std::size_t last = group_end(work, first);
        warn_group(work, std::span<const Claim>(work.claims.data() + first, last - first));
        first = last;
    }
}

} // namespace sunrise::state::mods::packages::internal

namespace sunrise::state::mods::packages {

/** A refused startup must never prune files named by the prior published overlay. */
void prune(const Paths& paths, const Result& published) noexcept {
    if (!published.complete) {
        return;
    }
    try {
        const std::wstring directory =
            core::path::extended(std::wstring(paths.mergeDirectory).c_str());
        if (directory.empty()) {
            return;
        }
        const std::wstring search = internal::join(directory, L"*");
        WIN32_FIND_DATAW entry{};
        const HANDLE listing = FindFirstFileW(search.c_str(), &entry);
        if (listing == INVALID_HANDLE_VALUE) {
            return;
        }
        directory_listing::Cursor cursor(listing);
        do {
            const std::wstring_view leaf(entry.cFileName);
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                || std::find(published.cacheLeaves.begin(), published.cacheLeaves.end(), leaf)
                       != published.cacheLeaves.end()) {
                continue;
            }
            (void)DeleteFileW(internal::join(directory, leaf).c_str());
        } while (FindNextFileW(listing, &entry) != FALSE);
        (void)cursor.finish(GetLastError());
    } catch (...) {}
}

} // namespace sunrise::state::mods::packages
