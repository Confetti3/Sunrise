#include <atomic>

#include "record_claims.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../build_data/nodes/definition.h"
#include "../build_data/nodes/node_catalog.h"
#include "../build_data/runtime.h"
#include "../unlocks/definition.h"
#include "objective_slot_table.h"
#include "parent_bar_table.h"

namespace sunrise::state::record_claims {
namespace {

/** One bit per addressable account flag index, which is cheaper than a set and never allocates. */
constexpr std::size_t kIndexCapacity = unlocks::kAccountFlagCapacity;
constexpr std::size_t kWordBits = 64;
constexpr std::size_t kWordCount = (kIndexCapacity + kWordBits - 1) / kWordBits;

/** The claim file lives beside the build data cache, which already owns this directory. */
constexpr std::wstring_view kClaimFileSuffix = L"\\cache\\record_claims.bin";
/** Identifies the file on sight, so an unrelated file of the right length cannot be read as one. */
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'C', 'L', 'M', '1'};
/** A claim is a flag bank row and the score its record carries. */
constexpr std::size_t kEntrySize = 2 * sizeof(std::uint16_t);
/** Far above the 2242 records the build ships, and small enough to read in one go. */
constexpr std::uint32_t kMaximumEntries = 8192;

/**
 * Completions that have not been claimed live in their own file, beside the claim file.
 *
 * Finding lore completes a record without claiming it, and that completion has to outlive the
 * process the same way a claim does -- otherwise every relaunch forgets what the player collected
 * and hands the same chapter out again. It is a separate file rather than a column added to the
 * claim file so the claim format keeps loading unchanged, including files written before this.
 */
constexpr std::wstring_view kClaimableFileSuffix = L"\\cache\\record_claimable.bin";
/** Distinct from kMagic so neither file can ever be read as the other. */
constexpr std::array<char, 8> kClaimableMagic{'S', 'N', 'R', 'S', 'C', 'M', 'P', '1'};

std::mutex g_lock;
std::array<std::uint64_t, kWordCount> g_claimed{};
/** Records complete but not yet claimed. A claim supersedes this, never the other way round. */
std::array<std::uint64_t, kWordCount> g_claimable{};
std::array<std::uint16_t, kIndexCapacity> g_scoreByIndex{};
std::size_t g_count{};
std::uint32_t g_score{};
core::path::Buffer g_path{};
bool g_pathReady{};
core::path::Buffer g_claimablePath{};
bool g_claimablePathReady{};

void report(const char* stage, const char* result, std::size_t detail) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=claims stage=%s result=%s entries=%zu", stage, result, detail);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         std::string_view{result} == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Writes every held claim. The caller holds the lock. */
void store_locked() noexcept {
    if (!g_pathReady) {
        return;
    }
    std::vector<char> document{};
    document.insert(document.end(), kMagic.begin(), kMagic.end());
    const auto entries = static_cast<std::uint32_t>(g_count);
    const auto* entryBytes = reinterpret_cast<const char*>(&entries);
    document.insert(document.end(), entryBytes, entryBytes + sizeof entries);
    for (std::size_t word = 0; word < g_claimed.size(); ++word) {
        std::uint64_t bits = g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const std::size_t index = word * kWordBits + offset;
            const auto packedIndex = static_cast<std::uint16_t>(index);
            const std::uint16_t packedScore = g_scoreByIndex[index];
            const auto* indexBytes = reinterpret_cast<const char*>(&packedIndex);
            const auto* scoreBytes = reinterpret_cast<const char*>(&packedScore);
            document.insert(document.end(), indexBytes, indexBytes + sizeof packedIndex);
            document.insert(document.end(), scoreBytes, scoreBytes + sizeof packedScore);
        }
    }

    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("store", "open_fail", g_count);
        return;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());
    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    report("store", complete ? "ok" : "write_fail", g_count);
}

/** Reads every claim the file holds. The caller holds the lock. */
void load_locked() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // No file is an account that has claimed nothing, which is the first-run state.
        report("load", "absent", 0);
        return;
    }
    std::array<char, sizeof(kMagic) + sizeof(std::uint32_t)> header{};
    DWORD read = 0;
    if (ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr) == FALSE
        || read != header.size()
        || std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
        (void)CloseHandle(file);
        report("load", "header_fail", 0);
        return;
    }
    std::uint32_t entries = 0;
    std::memcpy(&entries, header.data() + kMagic.size(), sizeof entries);
    if (entries > kMaximumEntries) {
        (void)CloseHandle(file);
        report("load", "count_fail", entries);
        return;
    }
    std::vector<char> payload(static_cast<std::size_t>(entries) * kEntrySize);
    read = 0;
    const bool readOk =
        payload.empty()
        || (ReadFile(file, payload.data(), static_cast<DWORD>(payload.size()), &read, nullptr)
                != FALSE
            && read == payload.size());
    (void)CloseHandle(file);
    if (!readOk) {
        report("load", "read_fail", entries);
        return;
    }

    std::size_t restored = 0;
    for (std::uint32_t entry = 0; entry < entries; ++entry) {
        std::uint16_t index = 0;
        std::uint16_t score = 0;
        std::memcpy(&index, payload.data() + static_cast<std::size_t>(entry) * kEntrySize,
                    sizeof index);
        std::memcpy(&score,
                    payload.data() + static_cast<std::size_t>(entry) * kEntrySize + sizeof index,
                    sizeof score);
        if (static_cast<std::size_t>(index) >= kIndexCapacity) {
            continue;
        }
        const std::size_t word = static_cast<std::size_t>(index) / kWordBits;
        const std::uint64_t bit = std::uint64_t{1}
                                  << (static_cast<std::size_t>(index) % kWordBits);
        if ((g_claimed[word] & bit) != 0) {
            continue;
        }
        g_claimed[word] |= bit;
        g_scoreByIndex[index] = score;
        g_score += score;
        ++g_count;
        ++restored;
    }
    report("load", "ok", restored);
}

/** Writes every completion that has not been claimed. The caller holds the lock. */
void store_claimable_locked() noexcept {
    if (!g_claimablePathReady) {
        return;
    }
    std::vector<char> document{};
    document.insert(document.end(), kClaimableMagic.begin(), kClaimableMagic.end());
    std::size_t held = 0;
    std::vector<char> entries{};
    for (std::size_t word = 0; word < g_claimable.size(); ++word) {
        // A claim is the later state, so a record that reached it needs no claimable row: the claim
        // file already carries it and load_locked restores it first.
        std::uint64_t bits = g_claimable[word] & ~g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const auto packedIndex = static_cast<std::uint16_t>(word * kWordBits + offset);
            constexpr std::uint16_t kUnscored = 0;
            const auto* indexBytes = reinterpret_cast<const char*>(&packedIndex);
            const auto* scoreBytes = reinterpret_cast<const char*>(&kUnscored);
            entries.insert(entries.end(), indexBytes, indexBytes + sizeof packedIndex);
            entries.insert(entries.end(), scoreBytes, scoreBytes + sizeof kUnscored);
            ++held;
        }
    }
    const auto count = static_cast<std::uint32_t>(held);
    const auto* countBytes = reinterpret_cast<const char*>(&count);
    document.insert(document.end(), countBytes, countBytes + sizeof count);
    document.insert(document.end(), entries.begin(), entries.end());

    const HANDLE file = CreateFileW(g_claimablePath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("store_claimable", "open_fail", held);
        return;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());
    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    report("store_claimable", complete ? "ok" : "write_fail", held);
}

/** Reads every unclaimed completion the file holds. The caller holds the lock. */
void load_claimable_locked() noexcept {
    const HANDLE file = CreateFileW(g_claimablePath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("load_claimable", "absent", 0);
        return;
    }
    std::array<char, sizeof(kClaimableMagic) + sizeof(std::uint32_t)> header{};
    DWORD read = 0;
    if (ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr) == FALSE
        || read != header.size()
        || std::memcmp(header.data(), kClaimableMagic.data(), kClaimableMagic.size()) != 0) {
        (void)CloseHandle(file);
        report("load_claimable", "header_fail", 0);
        return;
    }
    std::uint32_t entries = 0;
    std::memcpy(&entries, header.data() + kClaimableMagic.size(), sizeof entries);
    if (entries > kMaximumEntries) {
        (void)CloseHandle(file);
        report("load_claimable", "count_fail", entries);
        return;
    }
    std::vector<char> payload(static_cast<std::size_t>(entries) * kEntrySize);
    read = 0;
    const bool readOk =
        payload.empty()
        || (ReadFile(file, payload.data(), static_cast<DWORD>(payload.size()), &read, nullptr)
                != FALSE
            && read == payload.size());
    (void)CloseHandle(file);
    if (!readOk) {
        report("load_claimable", "read_fail", entries);
        return;
    }

    std::size_t restored = 0;
    for (std::uint32_t entry = 0; entry < entries; ++entry) {
        std::uint16_t index = 0;
        std::memcpy(&index, payload.data() + static_cast<std::size_t>(entry) * kEntrySize,
                    sizeof index);
        if (static_cast<std::size_t>(index) >= kIndexCapacity) {
            continue;
        }
        g_claimable[static_cast<std::size_t>(index) / kWordBits] |=
            std::uint64_t{1} << (static_cast<std::size_t>(index) % kWordBits);
        ++restored;
    }
    report("load_claimable", "ok", restored);
}

} // namespace

/** Derives the claim file path and loads any claims already held. */
bool initialize(void* module) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_pathReady = false;
    if (!core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kClaimFileSuffix)) {
        report("initialize", "path_fail", 0);
        return false;
    }
    g_pathReady = true;
    // Claims first: load_claimable_locked restores completions that were never claimed, and a
    // record that has since been claimed must already be known so it is not double counted.
    load_locked();

    g_claimablePathReady = false;
    if (core::path::artifact_directory(module, g_claimablePath)
        && core::path::append(g_claimablePath, kClaimableFileSuffix)) {
        g_claimablePathReady = true;
        load_claimable_locked();
    } else {
        report("initialize", "claimable_path_fail", 0);
    }
    return true;
}

/** Forgets every held claim, in memory only. */
void clear() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_claimed.fill(0);
    g_claimable.fill(0);
    g_scoreByIndex.fill(0);
    g_count = 0;
    g_score = 0;
}

/** Marks one account flag bank index claimed, adds its score, and writes the claim file. */
bool claim(std::uint16_t flagIndex, std::uint16_t scoreValue) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(flagIndex) / kWordBits;
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
    const std::lock_guard<std::mutex> guard(g_lock);
    if ((g_claimed[word] & bit) == 0) {
        g_claimed[word] |= bit;
        g_scoreByIndex[flagIndex] = scoreValue;
        ++g_count;
        // Only a first claim scores, so a repeated click cannot inflate the total.
        g_score += scoreValue;
        // Written per claim rather than at shutdown: a crash must not lose what the client is
        // already showing as Acquired.
        store_locked();
        // The claimable file lists completions still awaiting a claim, so this index has to leave
        // it now that the claim supersedes it -- otherwise it is carried in both files forever.
        store_claimable_locked();
    }
    return true;
}

/** Lays every held claim over one account flag bank. */
std::size_t apply(std::span<std::uint8_t> accountFlags) noexcept {
    std::size_t changed = 0;
    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::size_t word = 0; word < g_claimed.size(); ++word) {
        std::uint64_t bits = g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const std::size_t index = word * kWordBits + offset;
            // A bank shorter than the index space is not an error: the tail simply is not sent.
            if (index >= accountFlags.size()) {
                continue;
            }
            if (accountFlags[index] != unlocks::kFlagSet) {
                accountFlags[index] = unlocks::kFlagSet;
                ++changed;
            }
        }
    }

    // Claimable records are deliberately left alone here: the completion flag is a 2-bit
    // redeemed-only field with no value that means claimable (all four were measured), so a
    // claimable record's flag has to stay clear. What makes it read claimable is
    // apply_claimable_objectives writing its objective value(s) instead -- see that function.
    return changed;
}

namespace {

/** Carries the bank and a tally through the node walk, which takes a plain function pointer. */
struct NodeProgress {
    std::span<std::int32_t> values;
    std::size_t written;
    /** One entry per value index, non-zero where a category's own bar reads. */
    std::span<const char> categories;
};

/**
 * The objective slot run one record owns, or an empty span when the table does not name it.
 *
 * The slot space was derived and verified in game against thirteen measured points; see
 * objective_slot_table.h. A record's objectives occupy consecutive slots, so the run is found once
 * rather than a lookup per slot.
 */
[[nodiscard]] std::span<const objective_slot_table::ObjectiveSlot> objective_slots_for(
    std::uint16_t flagIndex) noexcept {
    const std::span<const objective_slot_table::RecordEntry> table{objective_slot_table::kRecords};
    const auto found = std::lower_bound(
        table.begin(), table.end(), flagIndex,
        [](const objective_slot_table::RecordEntry& entry, std::uint16_t key) {
            return entry.flagIndex < key;
        });
    if (found == table.end() || found->flagIndex != flagIndex) {
        return {};
    }
    return std::span<const objective_slot_table::ObjectiveSlot>{objective_slot_table::kObjectives}
        .subspan(found->firstObjective, found->objectiveCount);
}

/** True when this account flag bank row is held. The caller owns the claim lock. */
[[nodiscard]] bool claimed_locked(std::uint16_t flagIndex) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(flagIndex) / kWordBits;
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
    return (g_claimed[word] & bit) != 0;
}

/** True when this account flag bank row is complete but unclaimed. The caller owns the claim lock. */
[[nodiscard]] bool claimable_locked(std::uint16_t flagIndex) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::size_t word = static_cast<std::size_t>(flagIndex) / kWordBits;
    const std::uint64_t bit = std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
    return (g_claimable[word] & bit) != 0;
}

} // namespace

/**
 * @return How many of a category's children are claimed. The caller owns the claim lock.
 *
 * Both passes count the same thing against different banks, so they share the count rather than
 * each carrying a copy of the loop.
 */
[[nodiscard]] std::int32_t claimed_children(const build_data::nodes::Definition& node) noexcept {
    std::int32_t claimed = 0;
    for (std::size_t child = 0; child < node.childCount; ++child) {
        build_data::records::Definition record{};
        if (build_data::find_record_definition(node.children[child], record)
            && record.completionFlagIndex != build_data::records::kUnavailableFlagIndex
            && (claimed_locked(record.completionFlagIndex)
                || claimable_locked(record.completionFlagIndex))) {
            // A book's bar counts chapters FOUND, not chapters redeemed. Finding lore leaves a
            // record complete and unclaimed, so counting claims alone left every bar reading zero
            // however many chapters the player had collected.
            ++claimed;
        }
    }
    return claimed;
}

/**
 * @return One when a category's parent record is itself claimed, otherwise zero.
 *
 * The parent is the child naming the category's own value slot. It sits in the category's count and
 * not in its own bar's, so subtracting it is what separates the two totals. The caller owns the
 * claim lock.
 */
[[nodiscard]] std::int32_t claimed_parent(const build_data::nodes::Definition& node) noexcept {
    for (std::size_t child = 0; child < node.childCount; ++child) {
        build_data::records::Definition record{};
        if (build_data::find_record_definition(node.children[child], record)
            && record.completionFlagIndex != build_data::records::kUnavailableFlagIndex
            && record.categoryValueIndex == node.valueIndex
            && claimed_locked(record.completionFlagIndex)) {
            return 1;
        }
    }
    return 0;
}

/** Writes each node's claimed-child count into the value slot its bar reads. */
/**
 * Base the per-chapter visibility block is addressed from: a chapter's gate is kChapterGateBase
 * plus its own record row. Measured in game rather than derived -- the block was located by writing
 * markers across the bank and reading which chapters appeared, and this base is the unique fit for
 * four independent readings, including A Drifter's Gambit losing exactly its last chapter at one
 * segment edge and Most Loyal staying dark until the block was extended past the twenty-two rows
 * that separate it.
 */
constexpr std::int32_t kChapterGateBase = 1935;
/**
 * Rows 1-6 land on 1936-1941, which are parent bars of other books and already hold their counts.
 * Those satisfy the threshold on their own and must not be overwritten, so the block is only
 * written from here up.
 */
constexpr std::int32_t kChapterGateFirstWritable = 1942;
/** The last row whose chapter needs a gate. Beyond it every chapter is displayed by default. */
constexpr std::uint16_t kChapterGateLastRow = 106;

/** Publishes the per-chapter visibility gate of the Year 1 lore chapters. */
std::size_t apply_chapter_visibility_gates(std::span<std::int32_t> objectiveValues) noexcept {
    const std::span<const objective_slot_table::RecordEntry> table{objective_slot_table::kRecords};
    // The largest completion value any chapter in the block asks for. Read from the shipped table
    // rather than hard-coded: the corrupted-egg records count to nine and Truth to Power's last
    // chapter to eleven, and a gate written below a chapter's own requirement leaves it redacted.
    std::int32_t ceiling = 1;
    for (std::uint16_t row = 0; row <= kChapterGateLastRow; ++row) {
        build_data::records::Definition record{};
        if (!build_data::find_record_definition(row, record)
            || record.loreRow == build_data::records::kUnavailableLoreRow
            || record.completionFlagIndex == build_data::records::kUnavailableFlagIndex) {
            continue;
        }
        const auto found = std::lower_bound(
            table.begin(), table.end(), record.completionFlagIndex,
            [](const objective_slot_table::RecordEntry& entry, std::uint16_t flag) noexcept {
                return entry.flagIndex < flag;
            });
        if (found == table.end() || found->flagIndex != record.completionFlagIndex) {
            continue;
        }
        for (std::uint8_t i = 0; i < found->objectiveCount; ++i) {
            const std::size_t at = static_cast<std::size_t>(found->firstObjective) + i;
            if (at < objective_slot_table::kObjectives.size()) {
                ceiling = std::max(ceiling, objective_slot_table::kObjectives[at].completionValue);
            }
        }
    }

    // The block is filled uniformly rather than addressed per chapter. Which slot inside it belongs
    // to which chapter is NOT known: every measurement that located this block wrote one value
    // across a contiguous span, and a uniform span satisfies a chapter wherever it sits, so none of
    // them could distinguish the mapping. Addressing it as 1935 + record row was tried and is
    // wrong -- The Dreaming City's "Riven" vanished while the slots either side of its supposed one
    // stayed lit, and Truth to Power lost all eleven chapters that a uniform fill had shown.
    // Filling to the ceiling reproduces the state measured good; resolving the mapping needs a
    // per-slot sweep with distinct values and is worth doing before anything relies on it.
    std::size_t written = 0;
    const std::int32_t last = kChapterGateBase + static_cast<std::int32_t>(kChapterGateLastRow);
    for (std::int32_t slot = kChapterGateFirstWritable; slot <= last; ++slot) {
        if (static_cast<std::size_t>(slot) >= objectiveValues.size()) {
            break;
        }
        if (objectiveValues[static_cast<std::size_t>(slot)] < ceiling) {
            objectiveValues[static_cast<std::size_t>(slot)] = ceiling;
        }
        ++written;
    }
    return written;
}

std::size_t apply_node_progress(std::span<std::int32_t> objectiveValues) noexcept {
    // Every category's own index, so the walk can tell a free slot above a category from the next
    // category along. Without it, writing the slot above drives whichever book owns that slot.
    std::vector<char> categoryFlags(objectiveValues.size(), 0);
    build_data::nodes::for_each(
        &categoryFlags, [](void* context, const build_data::nodes::Definition& node) noexcept {
            auto* flags = static_cast<std::vector<char>*>(context);
            const std::uint16_t index = node.valueIndex;
            if (index != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::size_t>(index) < flags->size()) {
                (*flags)[index] = 1;
            }
        });

    NodeProgress progress{objectiveValues, 0, std::span<const char>{categoryFlags}};
    // The claim lock is taken first and the node lock inside the walk. Nothing takes them the other
    // way round, so the order cannot close a cycle.
    const std::lock_guard<std::mutex> guard(g_lock);
    build_data::nodes::for_each(
        &progress, [](void* context, const build_data::nodes::Definition& node) noexcept {
            auto* state = static_cast<NodeProgress*>(context);
            // Only the lore books are counted. Every other category that drives a bar keeps what
            // the authored policy gave it: node 896 carries an authored -1 at its value index, and
            // writing a count over it replaced a deliberate sentinel with a zero.
            if (!build_data::nodes::lore_category(node.definitionIndex)) {
                return;
            }
            // Counted from the records themselves, not from the node's own value slot: a book's
            // chapters are the children naming a lore row, and its parent triumph is the child
            // naming none. Both come from record rows, so neither needs the node's slot to have
            // resolved -- several books ship with no resolvable slot at all and were skipped
            // entirely while the count keyed on it.
            std::int32_t chapters = 0;
            std::int32_t collected = 0;
            build_data::records::Definition parent{};
            bool haveParent = false;
            for (std::size_t child = 0; child < node.childCount; ++child) {
                build_data::records::Definition record{};
                if (!build_data::find_record_definition(node.children[child], record)
                    || record.completionFlagIndex
                           == build_data::records::kUnavailableFlagIndex) {
                    continue;
                }
                if (record.loreRow == build_data::records::kUnavailableLoreRow) {
                    parent = record;
                    haveParent = true;
                    continue;
                }
                // Claimed only. Verified against the live game: a lore book's bar moves when the
                // chapter's triumph is claimed, not when the entry is collected. Counting
                // collected entries as well was tried on the reasoning that a record completes on
                // collection, and it is simply not what the bar does.
                if (claimed_locked(record.completionFlagIndex)) {
                    ++chapters;
                }
                // The category tile counts what has been collected, not what has been claimed.
                // It has to: the tile's counter is also the gate that reveals the book, and a
                // book that stayed hidden until a claim could never be opened at all, since a
                // chapter cannot be claimed while it is invisible.
                if (claimed_locked(record.completionFlagIndex)
                    || claimable_locked(record.completionFlagIndex)) {
                    ++collected;
                }
            }

            // The bar reads the value index the parent record's own expression names, read out of
            // the shipped tables rather than derived: writing the parent record's objective slot
            // was tried and moved nothing, and the node's valueIndex resolves for only some books.
            std::int32_t parentSlot = -1;
            {
                for (const auto& bar : parent_bar_table::kBars) {
                    if (bar.nodeIndex != node.definitionIndex) {
                        continue;
                    }
                    // A table entry naming the node's own gate is legitimate: ten books drive
                    // their bar from the very slot that gates the category, so the count belongs
                    // here even though writing zero into it would redact the book on its own.
                    // What prevents that is ordering, not a guard -- nodes::apply_category_gates
                    // runs after this pass and raises a zero gate back to one.
                    if (static_cast<std::size_t>(bar.valueIndex) < state->values.size()) {
                        state->values[bar.valueIndex] = chapters;
                        parentSlot = static_cast<std::int32_t>(bar.valueIndex);
                        ++state->written;
                    }
                    break;
                }
            }
            // The node's own value slot is the category tile's counter, and it is a different
            // number from the parent triumph's bar: the tile counts entries collected, the bar
            // counts triumphs claimed. Conflating them is what made ten books look as though
            // their gate were their bar -- writing the bar revealed them, but only because a
            // category with progress shows itself. On a cumulative book every chapter compares
            // against this counter too, so it has to carry the real total and not a token 1.
            // Kept below the record-objective range for the same reason apply_category_gates is:
            // three books name a slot inside it that belongs to a record's objective, and writing
            // a count there redacts records wholesale.
            if (node.valueIndex != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::int32_t>(node.valueIndex) < objective_slot_table::kRecordObjectiveRangeStart
                && static_cast<std::size_t>(node.valueIndex) < state->values.size()
                ) {
                // Collected, not claimed. This slot is the book's entries counter, and on a
                // cumulative book every chapter compares against it -- chapter n completes at n --
                // so a counter tracking claims leaves exactly one chapter claimable and the rest
                // locked, which is what claiming-only produced. It is only reached when the slot
                // is not also the book's bar: the guard above skips it when they coincide, and for
                // those ten the slot is the bar and has to keep counting claims.
                state->values[node.valueIndex] = chapters;
                ++state->written;
            }
            // Eight books name no value at field 136 and so have no table entry, but their parent
            // slot is still derivable from the shipped allocation -- the slot just past the run of
            // their children. That derivation drove their bars before this table existed and is
            // kept as the fallback, not replaced by it: the table is more trustworthy where it
            // applies, and this is the only source where it does not.
            if (parentSlot < 0
                && node.parentValueIndex != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::size_t>(node.parentValueIndex) < state->values.size()) {
                state->values[node.parentValueIndex] = chapters;
                parentSlot = static_cast<std::int32_t>(node.parentValueIndex);
                ++state->written;
            }
            std::array<char, 160> line{};
            const int told = std::snprintf(line.data(), line.size(),
                                           "ev=claims stage=node_bar node=%u children=%u "
                                           "chapters=%d parent_slot=%d value_slot=%u char_value=%u char_parent=%u",
                                           static_cast<unsigned>(node.definitionIndex),
                                           static_cast<unsigned>(node.childCount), chapters,
                                           parentSlot, static_cast<unsigned>(node.valueIndex),
                                           static_cast<unsigned>(node.characterValueIndex),
                                           static_cast<unsigned>(node.parentCharacterValueIndex));
            if (told > 0) {
                core::log::write(core::log::Channel::state, core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(told)});
            }
        });
    return progress.written;
}

/**
 * Writes each claimable-and-unclaimed record's authored objective value(s) into the objective
 * value bank.
 *
 * A triumph is claimable when its objective value equals its completionValue and its completion
 * flag is clear -- the flag has no value that means claimable, only claimed or nothing, so this is
 * the only bank that can carry the state. objective_slot_table maps a record's completion-flag
 * index to the slot(s) its objective(s) occupy; a multi-objective record's slots are consecutive,
 * found here as a run rather than one lookup per slot.
 */
std::size_t apply_claimable_objectives(std::span<std::int32_t> objectiveValues) noexcept {
    std::size_t written = 0;
    const std::span<const objective_slot_table::RecordEntry> table{objective_slot_table::kRecords};
    const std::lock_guard<std::mutex> guard(g_lock);
    for (std::size_t word = 0; word < g_claimable.size(); ++word) {
        // Only claimable-and-unclaimed records. Writing claimed ones too was tried and redacted
        // nearly every lore book: record objective slots share the 2746-5686 range with the value
        // indices some book gates read (4619, 4719, 4991 among them), so writing a value per claim
        // trampled those gates. A claim already shows through its completion flag.
        std::uint64_t bits = g_claimable[word] & ~g_claimed[word];
        while (bits != 0) {
            const auto offset = static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1;
            const auto flagIndex = static_cast<std::uint16_t>(word * kWordBits + offset);
            const auto found = std::lower_bound(
                table.begin(), table.end(), flagIndex,
                [](const objective_slot_table::RecordEntry& entry, std::uint16_t key) {
                    return entry.flagIndex < key;
                });
            if (found == table.end() || found->flagIndex != flagIndex) {
                // No objective slot for this record -- nothing this pass can write for it.
                continue;
            }
            for (std::uint8_t slot = 0; slot < found->objectiveCount; ++slot) {
                const std::size_t objectiveIndex =
                    static_cast<std::size_t>(found->firstObjective) + slot;
                if (objectiveIndex >= objective_slot_table::kObjectives.size()) {
                    break;
                }
                const objective_slot_table::ObjectiveSlot& objective =
                    objective_slot_table::kObjectives[objectiveIndex];
                // A bank shorter than the slot space is not an error: the tail simply is not sent.
                if (static_cast<std::size_t>(objective.slot) >= objectiveValues.size()) {
                    continue;
                }
                objectiveValues[objective.slot] = objective.completionValue;
                ++written;
            }
        }
    }
    return written;
}

/** Writes each category's claimed-child count into the character value slot its bar reads. */
std::size_t apply_character_node_progress(std::span<std::int32_t> characterValues) noexcept {
    NodeProgress progress{characterValues, 0, {}};
    // Same lock order as the account pass: claims first, catalog inside the walk.
    const std::lock_guard<std::mutex> guard(g_lock);
    build_data::nodes::for_each(
        &progress, [](void* context, const build_data::nodes::Definition& node) noexcept {
            auto* state = static_cast<NodeProgress*>(context);
            if (!build_data::nodes::lore_category(node.definitionIndex)) {
                return;
            }
            if (node.characterValueIndex != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::size_t>(node.characterValueIndex) < state->values.size()) {
                state->values[node.characterValueIndex] = claimed_children(node);
                ++state->written;
            }
            // The character-scoped books need their parent bar fed here too: their category
            // counts from this bank, and their parent's bar is character-scoped as well. The
            // parent index was resolved through the character value map at extraction.
            if (node.parentCharacterValueIndex != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::size_t>(node.parentCharacterValueIndex)
                       < state->values.size()) {
                state->values[node.parentCharacterValueIndex] =
                    claimed_children(node) - claimed_parent(node);
                ++state->written;
            }
        });
    return progress.written;
}

/** @return True when this index is already held. */
bool claimed(std::uint16_t flagIndex) noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return claimed_locked(flagIndex);
}

/** Marks one record complete but unclaimed. */
bool mark_claimable(std::uint16_t flagIndex) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    g_claimable[static_cast<std::size_t>(flagIndex) / kWordBits] |=
        1ULL << (static_cast<std::size_t>(flagIndex) % kWordBits);
    // Written through immediately, as mark_claimed does: a pickup is the player's progress and has
    // to survive the process, not just the session that recorded it.
    store_claimable_locked();
    return true;
}

/** @return True when this index is marked claimable. */
bool claimable(std::uint16_t flagIndex) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    return (g_claimable[static_cast<std::size_t>(flagIndex) / kWordBits]
            & (1ULL << (static_cast<std::size_t>(flagIndex) % kWordBits)))
           != 0;
}

/** @return Total score of every held claim. */
std::uint32_t total_score() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_score;
}

/** @return Number of distinct indices held. */
std::size_t count() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    return g_count;
}

} // namespace sunrise::state::record_claims
