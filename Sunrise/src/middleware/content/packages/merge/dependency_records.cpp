#include "dependency_records.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#include "../reader/layout.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::merge::dependency_records {
namespace {

using internal::Cursor;
using internal::load;

/** Misc fields: header size, root record count, then the record word array. */
struct MiscField final {
    // Offsets from the misc region start.
    static constexpr std::size_t kHeaderSize = 0x00;
    static constexpr std::size_t kRootCount = 0x30;
    static constexpr std::size_t kRecordCount = 0x40;
    static constexpr std::size_t kRecordArray = 0x48;
};

/** The record word array is a typed array; its count is its capacity. */
using ArrayField = reader::layout::TypedArrayHeader;

/** Record word flags; the low 29 bits are the body offset in the stream. */
constexpr std::uint32_t kGroupsFlag = 0x80000000U;
constexpr std::uint32_t kKeysFlag = 0x40000000U;
constexpr std::uint32_t kChildrenFlag = 0x20000000U;
constexpr std::uint32_t kOffsetMask = 0x1FFFFFFFU;
/** A run word with any of its top three bits set holds a run of 1 to 7 entries. */
constexpr unsigned kRunShift = 13;
constexpr std::uint16_t kShortRunMask = 0xE000;
constexpr std::uint32_t kShortRunLimit = 7;
/** Run starts keep 13 bits, the package entry limit. */
constexpr std::uint32_t kEntryMask = reader::layout::kTagEntryMask;
/** The stream starts, and key lists start, on 8-byte boundaries. */
constexpr std::size_t kStreamAlignment = 8;

/** One package's entries in a record, as encoded run words. */
struct Group final {
    std::uint16_t packageId{};
    std::vector<std::uint16_t> words;
};

/** One dependency record: groups, child records and keys, in that order. */
struct Record final {
    std::uint32_t flags{};
    std::uint32_t offset{};
    std::vector<Group> groups;
    std::vector<std::uint16_t> children;
    std::vector<std::uint64_t> keys;
};

/** The decoded record table of one misc region. */
struct Table final {
    std::vector<Record> records;
    /** Offset of record word 0. */
    std::size_t words{};
    /** Offset of the record stream. */
    std::size_t stream{};
};

/** @return `value` rounded up to the stream alignment. */
[[nodiscard]] std::size_t align(std::size_t value) noexcept {
    return static_cast<std::size_t>(internal::align(value, kStreamAlignment));
}

/**
 * Decodes one record body. Throws std::bad_alloc.
 * @return The body end relative to the stream, or nothing when it leaves the data.
 */
[[nodiscard]] std::optional<std::size_t>
parse(std::span<const std::byte> misc, std::size_t stream, Record& record) {
    Cursor cursor(misc, stream + record.offset);
    std::uint16_t count = 0;
    if ((record.flags & kGroupsFlag) != 0) {
        if (!cursor.take(count)) {
            return std::nullopt;
        }
        record.groups.resize(count);
        for (Group& group : record.groups) {
            std::uint16_t length = 0;
            if (!cursor.take(group.packageId) || !cursor.take(length)
                || !cursor.take(length, group.words)) {
                return std::nullopt;
            }
        }
    }
    if ((record.flags & kChildrenFlag) != 0
        && (!cursor.take(count) || !cursor.take(count, record.children))) {
        return std::nullopt;
    }
    if ((record.flags & kKeysFlag) != 0) {
        if (!cursor.take(count)) {
            return std::nullopt;
        }
        cursor.skip_to(stream + align(cursor.at() - stream));
        if (!cursor.take(count, record.keys)) {
            return std::nullopt;
        }
    }
    return cursor.at() - stream;
}

/** @return Every entry the run words mark, sorted; nothing when a run has no start word. */
[[nodiscard]] std::optional<std::vector<std::uint16_t>>
entries(std::span<const std::uint16_t> words) {
    std::vector<std::uint16_t> output;
    std::uint32_t pending = 0;
    for (const std::uint16_t word : words) {
        std::uint32_t run = 0;
        if (pending != 0) {
            run = pending;
            pending = 0;
        } else if ((word & kShortRunMask) != 0) {
            run = word >> kRunShift;
        } else {
            pending = word + 1U;
            continue;
        }
        const std::uint32_t start = word & kEntryMask;
        for (std::uint32_t i = 0; i < run; ++i) {
            output.push_back(static_cast<std::uint16_t>(start + i));
        }
    }
    if (pending != 0) {
        return std::nullopt;
    }
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return output;
}

/** A run of 1 to 7 is one word; a longer run is a length word, then a start word. */
[[nodiscard]] std::optional<std::vector<std::uint16_t>>
encode(std::span<const std::uint16_t> sorted) {
    std::vector<std::uint16_t> words;
    for (std::size_t i = 0; i < sorted.size();) {
        const std::uint32_t start = sorted[i];
        if (start > kEntryMask) {
            return std::nullopt;
        }
        std::uint32_t run = 1;
        while (i + run < sorted.size() && sorted[i + run] == start + run) {
            ++run;
        }
        if (run <= kShortRunLimit) {
            words.push_back(static_cast<std::uint16_t>((run << kRunShift) | start));
        } else {
            words.push_back(static_cast<std::uint16_t>(run - 1));
            words.push_back(static_cast<std::uint16_t>(start));
        }
        i += run;
    }
    return words;
}

void put16(std::vector<std::byte>& body, std::size_t value) {
    const auto word = static_cast<std::uint16_t>(value);
    const auto* bytes = reinterpret_cast<const std::byte*>(&word);
    body.insert(body.end(), bytes, bytes + sizeof word);
}

/** @return False when a count does not fit its 16-bit field. */
[[nodiscard]] bool encode_record(const Record& record, std::vector<std::byte>& body) {
    const auto fits = [](std::size_t count) {
        return count <= (std::numeric_limits<std::uint16_t>::max)();
    };
    if ((record.flags & kGroupsFlag) != 0) {
        if (!fits(record.groups.size())) {
            return false;
        }
        put16(body, record.groups.size());
        for (const Group& group : record.groups) {
            if (!fits(group.words.size())) {
                return false;
            }
            put16(body, group.packageId);
            put16(body, group.words.size());
            for (const std::uint16_t word : group.words) {
                put16(body, word);
            }
        }
    }
    if ((record.flags & kChildrenFlag) != 0) {
        if (!fits(record.children.size())) {
            return false;
        }
        put16(body, record.children.size());
        for (const std::uint16_t child : record.children) {
            put16(body, child);
        }
    }
    if ((record.flags & kKeysFlag) != 0) {
        if (!fits(record.keys.size())) {
            return false;
        }
        put16(body, record.keys.size());
        body.resize(align(body.size()));
        for (const std::uint64_t key : record.keys) {
            const auto* bytes = reinterpret_cast<const std::byte*>(&key);
            body.insert(body.end(), bytes, bytes + sizeof key);
        }
    }
    return true;
}

/** @return Live record indices in stream order; records made live keep offset 0. */
[[nodiscard]] std::vector<std::size_t> stream_order(const Table& table) {
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < table.records.size(); ++index) {
        if (table.records[index].flags != 0) {
            order.push_back(index);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return table.records[a].offset < table.records[b].offset;
    });
    return order;
}

/**
 * Encodes every record once, in stream order, after the unchanged prefix.
 * @return False when the stream outgrows the offset field.
 */
[[nodiscard]] bool
write(std::span<const std::byte> misc, const Table& table, std::vector<std::byte>& output) {
    std::vector<std::byte> body;
    std::vector<std::uint32_t> offsets(table.records.size(), 0);
    for (const std::size_t index : stream_order(table)) {
        offsets[index] = static_cast<std::uint32_t>(body.size());
        if (body.size() > kOffsetMask || !encode_record(table.records[index], body)) {
            return false;
        }
    }
    if (body.size() > kOffsetMask) {
        return false;
    }
    output.assign(misc.begin(), misc.begin() + static_cast<std::ptrdiff_t>(table.stream));
    output.insert(output.end(), body.begin(), body.end());
    for (std::size_t index = 0; index < table.records.size(); ++index) {
        const Record& record = table.records[index];
        const std::uint32_t word = record.flags == 0 ? 0 : record.flags | offsets[index];
        std::memcpy(output.data() + table.words + index * sizeof word, &word, sizeof word);
    }
    return true;
}

/** Result of reading one misc region. */
enum class Read : std::uint8_t {
    none,
    ok,
    bad,
};

/** @return True when the record word array header and the stream bounds hold. */
[[nodiscard]] bool locate(std::span<const std::byte> misc, std::uint64_t count, Table& table) {
    const auto relative = load<std::int64_t>(misc, MiscField::kRecordArray);
    if (relative < 0 || static_cast<std::uint64_t>(relative) > misc.size()) {
        return false;
    }
    const std::size_t header = MiscField::kRecordArray + static_cast<std::size_t>(relative);
    if (header < ArrayField::kMarkerBack || header + ArrayField::kSize > misc.size()
        || load<std::uint32_t>(misc, header - ArrayField::kMarkerBack)
               != reader::layout::kTableArrayMarker
        || load<std::uint32_t>(misc, header + ArrayField::kClass)
               != reader::layout::kU32ArrayClass) {
        return false;
    }
    const auto capacity = load<std::uint64_t>(misc, header + ArrayField::kCount);
    const auto size = load<std::uint64_t>(misc, MiscField::kHeaderSize);
    if (capacity < count || capacity > misc.size() || size > misc.size()) {
        return false;
    }
    table.words = header + ArrayField::kSize;
    table.stream = align(static_cast<std::size_t>(size));
    return table.words + capacity * sizeof(std::uint32_t) <= table.stream
           && table.stream <= misc.size();
}

/**
 * Decodes the record table and checks that it tiles its stream and re-encodes exactly.
 * Throws std::bad_alloc.
 */
[[nodiscard]] Read read(std::span<const std::byte> misc, Table& table) {
    if (misc.size() < MiscField::kRecordArray + sizeof(std::int64_t)) {
        return Read::none;
    }
    const auto count = load<std::int64_t>(misc, MiscField::kRecordCount);
    if (count == 0) {
        return Read::none;
    }
    if (count < 0 || !locate(misc, static_cast<std::uint64_t>(count), table)) {
        return Read::bad;
    }
    table.records.resize(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < table.records.size(); ++index) {
        const auto word = load<std::uint32_t>(misc, table.words + index * sizeof(std::uint32_t));
        Record& record = table.records[index];
        record.flags = word & ~kOffsetMask;
        record.offset = word & kOffsetMask;
    }
    const std::vector<std::size_t> order = stream_order(table);
    for (std::size_t i = 0; i < order.size(); ++i) {
        Record& record = table.records[order[i]];
        const auto end = parse(misc, table.stream, record);
        const std::size_t next =
            i + 1 < order.size() ? table.records[order[i + 1]].offset : misc.size() - table.stream;
        if (!end || *end != next) {
            return Read::bad;
        }
    }
    std::vector<std::byte> again;
    return write(misc, table, again)
                   && std::equal(again.begin(), again.end(), misc.begin(), misc.end())
               ? Read::ok
               : Read::bad;
}

/**
 * Adds one patch group's entries to a record of the merged table.
 * @return False when the entries cannot be decoded or encoded.
 */
[[nodiscard]] bool add(Record& record, const Group& patch) {
    const auto added = entries(patch.words);
    if (!added) {
        return false;
    }
    const auto found = std::find_if(record.groups.begin(),
                                    record.groups.end(),
                                    [&](const Group& g) { return g.packageId == patch.packageId; });
    if (found == record.groups.end()) {
        const auto at = std::find_if(record.groups.begin(),
                                     record.groups.end(),
                                     [&](const Group& g) { return g.packageId > patch.packageId; });
        const auto words = encode(*added);
        if (!words) {
            return false;
        }
        record.groups.insert(at, Group{patch.packageId, *words});
        record.flags |= kGroupsFlag;
        return true;
    }
    const auto current = entries(found->words);
    if (!current) {
        return false;
    }
    std::vector<std::uint16_t> combined = *current;
    combined.insert(combined.end(), added->begin(), added->end());
    std::sort(combined.begin(), combined.end());
    combined.erase(std::unique(combined.begin(), combined.end()), combined.end());
    // Unchanged groups keep their words, so an untouched record encodes as before.
    if (combined.size() == current->size()) {
        return true;
    }
    const auto words = encode(combined);
    if (!words) {
        return false;
    }
    found->words = *words;
    return true;
}

/** @return True when every base group entry is still listed by the patch record. */
[[nodiscard]] bool keeps_base(const Record& base, const Record& patch) {
    if ((base.flags & ~kGroupsFlag) != (patch.flags & ~kGroupsFlag)
        || base.children != patch.children || base.keys != patch.keys) {
        return false;
    }
    for (const Group& group : base.groups) {
        const auto match =
            std::find_if(patch.groups.begin(), patch.groups.end(), [&](const Group& g) {
                return g.packageId == group.packageId;
            });
        if (match == patch.groups.end()) {
            return false;
        }
        const auto before = entries(group.words);
        const auto after = entries(match->words);
        if (!before || !after
            || !std::includes(after->begin(), after->end(), before->begin(), before->end())) {
            return false;
        }
    }
    return true;
}

/** @return True when the patch prefix matches the base outside the record words. */
[[nodiscard]] bool same_prefix(std::span<const std::byte> base,
                               std::span<const std::byte> patch,
                               const Table& table) noexcept {
    const std::size_t wordsEnd = table.words + table.records.size() * sizeof(std::uint32_t);
    return patch.size() >= table.stream
           && std::equal(
               base.begin(), base.begin() + static_cast<std::ptrdiff_t>(table.words), patch.begin())
           && std::equal(base.begin() + static_cast<std::ptrdiff_t>(wordsEnd),
                         base.begin() + static_cast<std::ptrdiff_t>(table.stream),
                         patch.begin() + static_cast<std::ptrdiff_t>(wordsEnd));
}

/** Throws std::bad_alloc. @return ok, or names. */
[[nodiscard]] Status union_records(std::span<const std::byte> base,
                                   std::span<const std::span<const std::byte>> patches,
                                   std::vector<std::byte>& output) {
    const auto unchanged = [&](std::span<const std::byte> patch) {
        return std::equal(patch.begin(), patch.end(), base.begin(), base.end());
    };
    if (std::all_of(patches.begin(), patches.end(), unchanged)) {
        output.assign(base.begin(), base.end());
        return Status::ok;
    }
    Table merged;
    if (read(base, merged) != Read::ok) {
        return Status::names;
    }
    const Table original = merged;
    for (const std::span<const std::byte> patch : patches) {
        Table table;
        if (!same_prefix(base, patch, original) || read(patch, table) != Read::ok
            || table.records.size() != original.records.size()) {
            return Status::names;
        }
        for (std::size_t index = 0; index < table.records.size(); ++index) {
            if (!keeps_base(original.records[index], table.records[index])) {
                return Status::names;
            }
            for (const Group& group : table.records[index].groups) {
                if (!add(merged.records[index], group)) {
                    return Status::names;
                }
            }
        }
    }
    return write(base, merged, output) ? Status::ok : Status::names;
}

} // namespace

/** Any difference beyond added record entries returns names. */
Status merge(std::span<const std::byte> base,
             std::span<const std::span<const std::byte>> patches,
             std::vector<std::byte>& output) noexcept {
    try {
        output.clear();
        return union_records(base, patches, output);
    } catch (...) {
        output.clear();
        return Status::capacity;
    }
}

} // namespace sunrise::middleware::content::packages::merge::dependency_records
