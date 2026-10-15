#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "internal.h"

namespace sunrise::middleware::content::packages::table_merge::internal {
namespace {

using Bytes = std::span<const std::byte>;
/** One index space: a table tag and the descriptor offset of its array. */
using TargetKey = std::pair<std::uint32_t, std::uint32_t>;

/** A site width counts bytes; its range counts bits. */
constexpr std::uint32_t kBitsPerByte = 8;

/** @return The top-level append op at `descriptor` one input declares, or null. */
[[nodiscard]] const OpRecord*
top_op(const Input& input, std::uint32_t entryIndex, std::uint32_t descriptor) noexcept {
    const Entry* entry = entry_of(input, entryIndex);
    if (entry == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(entry->ops.begin(), entry->ops.end(), [&](const OpRecord& op) {
        return op.parentOp == kTopLevelOp && op.kind == OpKind::append
               && op.descriptorOffset == descriptor;
    });
    return found == entry->ops.end() ? nullptr : &*found;
}

/** @return The smallest and largest value one site can hold. */
[[nodiscard]] std::pair<std::int64_t, std::int64_t> site_range(const SiteRecord& site) noexcept {
    const std::uint32_t bits = site.width * kBitsPerByte;
    if ((site.flags & kSignedSite) != 0) {
        const std::int64_t half = std::int64_t{1} << (bits - 1);
        return {-half, half - 1};
    }
    return {0, (std::int64_t{1} << bits) - 1};
}

/** @return The site value at its width and sign. The caller has checked the bounds. */
[[nodiscard]] std::int64_t read_site(Bytes bytes, const SiteRecord& site) noexcept {
    std::uint32_t raw = 0;
    std::memcpy(&raw, bytes.data() + site.offset, site.width);
    const std::uint32_t bits = site.width * kBitsPerByte;
    const std::int64_t value = raw;
    // Sign-extend from the site's own width.
    const bool negative = (site.flags & kSignedSite) != 0 && ((raw >> (bits - 1)) & 1U) != 0;
    return negative ? value - (std::int64_t{1} << bits) : value;
}

/** @return The [stockCount, modCount) range a site must name a row in. */
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
row_range(const Sidecar& sidecar, const Entry& entry, const SiteRecord& site) noexcept {
    if ((site.target & kOpLocalTarget) != 0) {
        const OpRecord& op = entry.ops[site.target & kOpLocalMask];
        return {op.stockCount, op.modCount};
    }
    const TargetRecord& target = sidecar.targets()[site.target];
    return {target.stockCount, target.modCount};
}

/** Plans the row shifts across every entry in a merge. */
class Planner final {
public:
    Planner(std::span<const EntryInputs> entries, Shifts& shifts) noexcept
        : entries_(entries), shifts_(shifts) {}

    /** @param failedTable Receives the tag of the table that failed. @return ok, or why. */
    [[nodiscard]] Status run(std::uint32_t& failedTable) {
        const Status status = collect(failedTable);
        if (status != Status::ok) {
            return status;
        }
        for (const auto& [target, stockCount] : stockCounts_) {
            const Status shifted = shift(target, stockCount);
            if (shifted != Status::ok) {
                failedTable = target.first;
                return shifted;
            }
        }
        return check_pairs(failedTable);
    }

private:
    Status collect(std::uint32_t& failedTable);
    Status shift(const TargetKey& target, std::uint32_t stockCount);
    Status check_pairs(std::uint32_t& failedTable) const;

    std::span<const EntryInputs> entries_;
    Shifts& shifts_;
    std::map<std::pair<std::uint16_t, std::uint32_t>, const EntryInputs*> byEntry_;
    std::map<TargetKey, std::uint32_t> stockCounts_;
    /** Each mod's final count per target, keyed (tag, descriptor, mod). */
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::size_t>, std::uint32_t> records_;
};

/** Indexes the entries and every mod's target records; one target keeps one stock count. */
Status Planner::collect(std::uint32_t& failedTable) {
    std::set<std::pair<std::size_t, const Sidecar*>> seen;
    for (const EntryInputs& entry : entries_) {
        byEntry_.try_emplace({entry.packageId, entry.entryIndex}, &entry);
        for (const Input& input : entry.inputs) {
            // One sidecar serves every entry of its package, so its targets are read once.
            if (input.sidecar == nullptr || !seen.insert({input.mod, input.sidecar}).second) {
                continue;
            }
            for (const TargetRecord& target : input.sidecar->targets()) {
                const TargetKey key{target.tableTag, target.descriptorOffset};
                const auto stock = stockCounts_.try_emplace(key, target.stockCount).first;
                const auto record =
                    records_.try_emplace({key.first, key.second, input.mod}, target.modCount);
                if (stock->second != target.stockCount || record.first->second != target.modCount) {
                    failedTable = key.first;
                    return Status::mismatch;
                }
            }
        }
    }
    return Status::ok;
}

/**
 * Gives every contributor to one target the rows that earlier contributors added.
 * @return ok, mismatch when counts disagree, or limit when the merged count is too large.
 */
Status Planner::shift(const TargetKey& target, std::uint32_t stockCount) {
    const std::uint32_t tag = target.first;
    if (tag < reader::layout::kTagBase) {
        return Status::mismatch;
    }
    const std::uint32_t package = reader::layout::package_of(tag);
    if (package > (std::numeric_limits<std::uint16_t>::max)()) {
        return Status::mismatch;
    }
    const auto found =
        byEntry_.find({static_cast<std::uint16_t>(package), reader::layout::entry_of_tag(tag)});
    const auto first = records_.lower_bound({tag, target.second, 0});
    const auto inTarget = [&](const auto& record) {
        return record != records_.end() && std::get<0>(record->first) == tag
               && std::get<1>(record->first) == target.second;
    };
    // An entry outside the merge has one contributor at most, so nothing moves.
    // Its count changes in one mod's records at most.
    if (found == byEntry_.end()) {
        std::size_t changed = 0;
        for (auto record = first; inTarget(record); ++record) {
            changed += record->second != stockCount ? 1U : 0U;
        }
        return changed > 1 ? Status::mismatch : Status::ok;
    }
    std::uint64_t added = 0;
    const OpRecord* declared = nullptr;
    std::map<std::size_t, std::uint32_t> carried;
    for (const Input& input : found->second->inputs) {
        if (added != 0) {
            shifts_.set(input.mod, tag, target.second, static_cast<std::uint32_t>(added));
        }
        const OpRecord* op = top_op(input, found->second->entryIndex, target.second);
        if (op == nullptr) {
            continue;
        }
        // Every declaring mod must give the same limit.
        declared = declared == nullptr ? op : declared;
        if (op->stockCount != stockCount || op->limit != declared->limit) {
            return Status::mismatch;
        }
        added += op->modCount - op->stockCount;
        carried[input.mod] = op->modCount;
    }
    if (declared != nullptr && stockCount + added > declared->limit) {
        return Status::limit;
    }
    // Each mod's own count must match the op it ships, or the stock count when it ships none.
    for (auto record = first; inTarget(record); ++record) {
        const auto own = carried.find(std::get<2>(record->first));
        if (record->second != (own == carried.end() ? stockCount : own->second)) {
            return Status::mismatch;
        }
    }
    return Status::ok;
}

/**
 * Checks that each mod adds the same row count to every member of each pair group.
 * A member a mod does not change counts as 0.
 * @return ok, or pairs with the tag of the member that disagrees.
 */
Status Planner::check_pairs(std::uint32_t& failedTable) const {
    std::map<std::uint32_t, std::set<TargetKey>> members;
    std::map<std::pair<std::uint32_t, std::size_t>, std::map<TargetKey, std::uint32_t>> added;
    std::set<std::pair<std::size_t, const Sidecar*>> seen;
    // Every entry of each sidecar counts, so a member outside the merge is still checked.
    for (const EntryInputs& entry : entries_) {
        for (const Input& input : entry.inputs) {
            if (input.sidecar == nullptr || !seen.insert({input.mod, input.sidecar}).second) {
                continue;
            }
            for (const Entry& spec : input.sidecar->entries()) {
                const std::uint32_t tag =
                    tag_of(input.sidecar->header().packageId, spec.header.entryIndex);
                for (const OpRecord& op : spec.ops) {
                    if (op.parentOp != kTopLevelOp || op.kind != OpKind::append
                        || op.pairGroup == 0) {
                        continue;
                    }
                    members[op.pairGroup].insert({tag, op.descriptorOffset});
                    added[{op.pairGroup, input.mod}][{tag, op.descriptorOffset}] =
                        op.modCount - op.stockCount;
                }
            }
        }
    }
    for (const auto& [key, counts] : added) {
        const auto& group = members.at(key.first);
        const auto first = counts.find(*group.begin());
        const std::uint32_t expected = first == counts.end() ? 0 : first->second;
        for (const TargetKey& member : group) {
            const auto own = counts.find(member);
            if ((own == counts.end() ? 0 : own->second) != expected) {
                failedTable = member.first;
                return Status::pairs;
            }
        }
    }
    return Status::ok;
}

} // namespace

/** Plans every mod's shift for every target, from the sidecars only. */
Status
plan_shifts(std::span<const EntryInputs> entries, Shifts& shifts, std::uint32_t& failedTable) {
    failedTable = 0;
    shifts.clear();
    Planner planner(entries, shifts);
    return planner.run(failedTable);
}

/** Checks that every site value names a row its mod added. */
Status check_sites(const Input& input, std::uint32_t entryIndex) noexcept {
    const Entry* entry = entry_of(input, entryIndex);
    if (entry == nullptr) {
        return Status::ok;
    }
    for (const SiteRecord& site : entry->sites) {
        // Only append rows keep their numbers; sorted map rows move by key.
        const bool local = (site.target & kOpLocalTarget) != 0;
        if (std::uint64_t{site.offset} + site.width > input.bytes.size()
            || (local && entry->ops[site.target & kOpLocalMask].kind != OpKind::append)) {
            return Status::sites;
        }
        const auto [first, end] = row_range(*input.sidecar, *entry, site);
        const std::int64_t value = read_site(input.bytes, site);
        if (value < first || value >= end) {
            return Status::sites;
        }
    }
    return Status::ok;
}

/** Adds each site's planned shift in place. */
Status shift_sites(const Input& input,
                   std::uint32_t entryIndex,
                   const Shifts& shifts,
                   std::span<const std::uint32_t> opLocalShifts,
                   std::vector<std::byte>& copy) {
    const Entry* entry = entry_of(input, entryIndex);
    if (entry == nullptr) {
        return Status::ok;
    }
    for (const SiteRecord& site : entry->sites) {
        std::uint32_t rows = 0;
        if ((site.target & kOpLocalTarget) != 0) {
            const std::size_t op = site.target & kOpLocalMask;
            rows = op < opLocalShifts.size() ? opLocalShifts[op] : 0;
        } else {
            const TargetRecord& target = input.sidecar->targets()[site.target];
            rows = shifts.get(input.mod, target.tableTag, target.descriptorOffset);
        }
        if (rows == 0) {
            continue;
        }
        if (std::uint64_t{site.offset} + site.width > copy.size()) {
            return Status::format;
        }
        const std::int64_t value = read_site(copy, site) + rows;
        if (value > site_range(site).second) {
            return Status::limit;
        }
        // Little-endian two's complement: the low `width` bytes are the value.
        const auto raw = static_cast<std::uint32_t>(value);
        std::memcpy(copy.data() + site.offset, &raw, site.width);
    }
    return Status::ok;
}

} // namespace sunrise::middleware::content::packages::table_merge::internal
