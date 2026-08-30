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
 * Books whose shared progress counter grants chapters directly.
 *
 * Unlike collectible-backed lore, these chapters never become separately claimable records. The
 * counter is both the number of entries collected and the parent bar value.
 */
constexpr std::array<std::uint16_t, 4> kCounterGrantedLoreNodes{
    823U,  // Stolen Intelligence
    839U,  // Unveiling
    850U,  // A Man with No Name
    853U,  // Revelation
};

[[nodiscard]] constexpr bool counter_granted_lore(std::uint16_t nodeIndex) noexcept {
    return std::find(kCounterGrantedLoreNodes.begin(), kCounterGrantedLoreNodes.end(), nodeIndex)
           != kCounterGrantedLoreNodes.end();
}

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
/** Partial single-objective values live apart from completion and claim state. */
constexpr std::wstring_view kProgressFileSuffix = L"\\cache\\record_progress.bin";
constexpr std::array<char, 8> kProgressMagic{'S', 'N', 'R', 'S', 'P', 'R', 'G', '1'};
constexpr std::size_t kProgressEntrySize = sizeof(std::uint16_t) + sizeof(std::int32_t);

std::mutex g_lock;
std::array<std::uint64_t, kWordCount> g_claimed{};
/** Records complete but not yet claimed. A claim supersedes this, never the other way round. */
std::array<std::uint64_t, kWordCount> g_claimable{};
/** Partial progress keyed by completion-flag index; zero means no persisted partial value. */
std::array<std::int32_t, kIndexCapacity> g_progress{};
std::array<std::uint16_t, kIndexCapacity> g_scoreByIndex{};
std::size_t g_count{};
std::uint32_t g_score{};
core::path::Buffer g_path{};
bool g_pathReady{};
core::path::Buffer g_claimablePath{};
bool g_claimablePathReady{};
core::path::Buffer g_progressPath{};
bool g_progressPathReady{};

/** Objective storage reserved by records whose installed definition exposes no objective rows. */
struct ReservedObjective {
    std::uint16_t flagIndex;
    std::uint16_t firstSlot;
    std::uint8_t slotCount;
    std::int32_t completionValue;
};

constexpr std::array<ReservedObjective, 1> kReservedObjectives{{
    {9448U, 3432U, 2U, 9}, // Remember Your Manners
}};

[[nodiscard]] const ReservedObjective*
find_reserved_objective(std::uint16_t flagIndex) noexcept {
    const auto found = std::find_if(
        kReservedObjectives.begin(),
        kReservedObjectives.end(),
        [flagIndex](const ReservedObjective& objective) { return objective.flagIndex == flagIndex; });
    return found != kReservedObjectives.end() ? &*found : nullptr;
}

[[nodiscard]] bool claimed_locked(std::uint16_t flagIndex) noexcept;
[[nodiscard]] bool claimable_locked(std::uint16_t flagIndex) noexcept;

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

/** Writes every nonzero partial objective value. The caller holds the lock. */
void store_progress_locked() noexcept {
    if (!g_progressPathReady) {
        return;
    }
    std::vector<char> entries{};
    std::uint32_t count = 0;
    for (std::size_t index = 0; index < g_progress.size(); ++index) {
        if (g_progress[index] <= 0 || claimed_locked(static_cast<std::uint16_t>(index))
            || claimable_locked(static_cast<std::uint16_t>(index))) {
            continue;
        }
        const auto packedIndex = static_cast<std::uint16_t>(index);
        const auto* indexBytes = reinterpret_cast<const char*>(&packedIndex);
        const auto* valueBytes = reinterpret_cast<const char*>(&g_progress[index]);
        entries.insert(entries.end(), indexBytes, indexBytes + sizeof packedIndex);
        entries.insert(entries.end(), valueBytes, valueBytes + sizeof g_progress[index]);
        ++count;
    }
    std::vector<char> document{};
    document.insert(document.end(), kProgressMagic.begin(), kProgressMagic.end());
    const auto* countBytes = reinterpret_cast<const char*>(&count);
    document.insert(document.end(), countBytes, countBytes + sizeof count);
    document.insert(document.end(), entries.begin(), entries.end());

    const HANDLE file = CreateFileW(g_progressPath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("store_progress", "open_fail", count);
        return;
    }
    DWORD written = 0;
    const auto size = static_cast<DWORD>(document.size());
    bool complete =
        WriteFile(file, document.data(), size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    report("store_progress", complete ? "ok" : "write_fail", count);
}

/** Reads persisted partial objective values. The caller holds the lock. */
void load_progress_locked() noexcept {
    const HANDLE file = CreateFileW(g_progressPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("load_progress", "absent", 0);
        return;
    }
    std::array<char, sizeof(kProgressMagic) + sizeof(std::uint32_t)> header{};
    DWORD read = 0;
    if (ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read, nullptr) == FALSE
        || read != header.size()
        || std::memcmp(header.data(), kProgressMagic.data(), kProgressMagic.size()) != 0) {
        (void)CloseHandle(file);
        report("load_progress", "header_fail", 0);
        return;
    }
    std::uint32_t entries = 0;
    std::memcpy(&entries, header.data() + kProgressMagic.size(), sizeof entries);
    if (entries > kMaximumEntries) {
        (void)CloseHandle(file);
        report("load_progress", "count_fail", entries);
        return;
    }
    std::vector<char> payload(static_cast<std::size_t>(entries) * kProgressEntrySize);
    read = 0;
    const bool readOk =
        payload.empty()
        || (ReadFile(file, payload.data(), static_cast<DWORD>(payload.size()), &read, nullptr)
                != FALSE
            && read == payload.size());
    (void)CloseHandle(file);
    if (!readOk) {
        report("load_progress", "read_fail", entries);
        return;
    }

    std::size_t restored = 0;
    for (std::uint32_t entry = 0; entry < entries; ++entry) {
        const std::size_t at = static_cast<std::size_t>(entry) * kProgressEntrySize;
        std::uint16_t index = 0;
        std::int32_t value = 0;
        std::memcpy(&index, payload.data() + at, sizeof index);
        std::memcpy(&value, payload.data() + at + sizeof index, sizeof value);
        if (static_cast<std::size_t>(index) >= kIndexCapacity || value <= 0
            || claimed_locked(index) || claimable_locked(index)) {
            continue;
        }
        g_progress[index] = value;
        ++restored;
    }
    report("load_progress", "ok", restored);
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

    g_progressPathReady = false;
    if (core::path::artifact_directory(module, g_progressPath)
        && core::path::append(g_progressPath, kProgressFileSuffix)) {
        g_progressPathReady = true;
        load_progress_locked();
    } else {
        report("initialize", "progress_path_fail", 0);
    }
    return true;
}

/** Forgets every held claim, in memory only. */
void clear() noexcept {
    const std::lock_guard<std::mutex> guard(g_lock);
    g_claimed.fill(0);
    g_claimable.fill(0);
    g_progress.fill(0);
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
        g_progress[flagIndex] = 0;
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
        store_progress_locked();
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

/** Clears the authored completion values of every record owned by a lore book. */
std::size_t clear_lore_objectives(std::span<std::int32_t> objectiveValues) noexcept {
    struct ClearState {
        std::span<std::int32_t> values;
        std::size_t cleared{};
    } state{objectiveValues};
    build_data::nodes::for_each(
        &state, [](void* context, const build_data::nodes::Definition& node) noexcept {
            if (!build_data::nodes::lore_category(node.definitionIndex)) {
                return;
            }
            auto* clear = static_cast<ClearState*>(context);
            for (std::size_t child = 0; child < node.childCount; ++child) {
                build_data::records::Definition record{};
                if (!build_data::find_record_definition(node.children[child], record)
                    || record.completionFlagIndex
                           == build_data::records::kUnavailableFlagIndex) {
                    continue;
                }
                const auto found = std::lower_bound(
                    objective_slot_table::kRecords.begin(), objective_slot_table::kRecords.end(),
                    record.completionFlagIndex,
                    [](const objective_slot_table::RecordEntry& entry, std::uint16_t flag) {
                        return entry.flagIndex < flag;
                    });
                if (found == objective_slot_table::kRecords.end()
                    || found->flagIndex != record.completionFlagIndex) {
                    continue;
                }
                for (std::uint8_t objective = 0; objective < found->objectiveCount; ++objective) {
                    const std::size_t at =
                        static_cast<std::size_t>(found->firstObjective) + objective;
                    if (at >= objective_slot_table::kObjectives.size()) {
                        break;
                    }
                    const std::size_t slot = objective_slot_table::kObjectives[at].slot;
                    if (slot >= clear->values.size()) {
                        continue;
                    }
                    clear->values[slot] = 0;
                    ++clear->cleared;
                }
            }
        });
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        report("clear_lore_objectives", "ok", state.cleared);
    }
    return state.cleared;
}

namespace {

/** Carries the bank and a tally through the node walk, which takes a plain function pointer. */
struct NodeProgress {
    std::span<std::int32_t> values;
    std::size_t written;
};

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

/**
 * Builds the completion-flag mask for counter-granted chapters. The caller owns the claim lock.
 *
 * Their persisted completion bits are still the collection ledger, so relaunches retain the
 * counter. They are masked only from ordinary objective emission: native content derives their
 * collected state from the shared counter and never offers the chapter records for claiming.
 */
[[nodiscard]] std::array<std::uint64_t, kWordCount>
counter_granted_chapter_mask_locked() noexcept {
    std::array<std::uint64_t, kWordCount> mask{};
    build_data::nodes::for_each(
        &mask, [](void* context, const build_data::nodes::Definition& node) noexcept {
            if (!counter_granted_lore(node.definitionIndex)) {
                return;
            }
            auto* bits = static_cast<std::array<std::uint64_t, kWordCount>*>(context);
            for (std::size_t child = 0; child < node.childCount; ++child) {
                build_data::records::Definition record{};
                if (!build_data::find_record_definition(node.children[child], record)
                    || record.loreRow == build_data::records::kUnavailableLoreRow
                    || static_cast<std::size_t>(record.completionFlagIndex) >= kIndexCapacity) {
                    continue;
                }
                const std::size_t index = record.completionFlagIndex;
                (*bits)[index / kWordBits] |= std::uint64_t{1} << (index % kWordBits);
            }
        });
    return mask;
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

/** Publishes the per-chapter visibility gate only for Year 1 lore chapters already collected. */
std::size_t apply_chapter_visibility_gates(std::span<std::int32_t> objectiveValues) noexcept {
    const std::span<const objective_slot_table::RecordEntry> table{objective_slot_table::kRecords};
    const std::lock_guard<std::mutex> guard(g_lock);
    std::size_t written = 0;
    for (std::uint16_t row = 0; row <= kChapterGateLastRow; ++row) {
        build_data::records::Definition record{};
        if (!build_data::find_record_definition(row, record)
            || record.loreRow == build_data::records::kUnavailableLoreRow
            || record.completionFlagIndex == build_data::records::kUnavailableFlagIndex
            || (!claimed_locked(record.completionFlagIndex)
                && !claimable_locked(record.completionFlagIndex))) {
            continue;
        }
        const auto found = std::lower_bound(
            table.begin(), table.end(), record.completionFlagIndex,
            [](const objective_slot_table::RecordEntry& entry, std::uint16_t flag) noexcept {
                return entry.flagIndex < flag;
            });
        if (found == table.end() || found->flagIndex != record.completionFlagIndex
            || found->objectiveCount == 0) {
            continue;
        }
        const std::size_t objective = found->firstObjective;
        const std::size_t gate = static_cast<std::size_t>(kChapterGateBase) + row;
        if (row < static_cast<std::uint16_t>(kChapterGateFirstWritable - kChapterGateBase)
            || objective >= objective_slot_table::kObjectives.size()
            || gate >= objectiveValues.size()) {
            continue;
        }
        const std::int32_t completion =
            objective_slot_table::kObjectives[objective].completionValue;
        if (objectiveValues[gate] < completion) {
            objectiveValues[gate] = completion;
        }
        ++written;
    }
    return written;
}

std::size_t apply_node_progress(std::span<std::int32_t> objectiveValues) noexcept {
    NodeProgress progress{objectiveValues, 0};
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
            // A cumulative book's chapter n completes at value n rather than 1, and every one of
            // them compares against this node's counter, so it must carry the real total or the
            // chapters stay hidden. A book whose chapters all complete at 1 needs nothing here --
            // Dust reads 0 with all nine collected and every chapter still claimable -- so those
            // keep the sentinel and their bar stays honest.
            bool cumulative = false;
            const bool counterGranted = counter_granted_lore(node.definitionIndex);
            for (std::size_t child = 0; child < node.childCount; ++child) {
                build_data::records::Definition record{};
                if (!build_data::find_record_definition(node.children[child], record)
                    || record.completionFlagIndex
                           == build_data::records::kUnavailableFlagIndex) {
                    continue;
                }
                if (record.loreRow == build_data::records::kUnavailableLoreRow) {
                    continue;
                }
                // Most books count chapter triumph claims. Four activity/vendor books are
                // different: advancing their parent counter is the act that grants each entry.
                if (claimed_locked(record.completionFlagIndex)) {
                    ++chapters;
                }
                // The completion set is also the persistent collection ledger. For ordinary lore
                // it means complete-but-unclaimed; for counter-granted lore it records an entry
                // already granted by the shared counter and is never emitted as a claimable row.
                if (claimed_locked(record.completionFlagIndex)
                    || claimable_locked(record.completionFlagIndex)) {
                    ++collected;
                }
                {
                    const auto entry = std::lower_bound(
                        objective_slot_table::kRecords.begin(),
                        objective_slot_table::kRecords.end(), record.completionFlagIndex,
                        [](const objective_slot_table::RecordEntry& row,
                           std::uint16_t flag) noexcept { return row.flagIndex < flag; });
                    if (entry != objective_slot_table::kRecords.end()
                        && entry->flagIndex == record.completionFlagIndex
                        && entry->objectiveCount != 0) {
                        const std::size_t at = entry->firstObjective;
                        if (at < objective_slot_table::kObjectives.size()
                            && objective_slot_table::kObjectives[at].completionValue > 1) {
                            cumulative = true;
                        }
                    }
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
                        state->values[bar.valueIndex] = counterGranted ? collected : chapters;
                        parentSlot = static_cast<std::int32_t>(bar.valueIndex);
                        ++state->written;
                    }
                    break;
                }
            }
            // The node's own value slot normally carries the category's collection count while
            // the parent bar carries claims. Cumulative chapters compare against the former, so
            // it must carry the collected total rather than a visibility token. The four
            // counter-granted books deliberately collapse those meanings: their node value is
            // also the parent bar, and incrementing it is what grants the next chapter.
            // Kept below the record-objective range for the same reason apply_category_gates is:
            // three books name a slot inside it that belongs to a record's objective, and writing
            // a count there redacts records wholesale.
            if (node.valueIndex != build_data::nodes::kUnavailableValueIndex
                && static_cast<std::int32_t>(node.valueIndex) < objective_slot_table::kRecordObjectiveRangeStart
                && static_cast<std::size_t>(node.valueIndex) < state->values.size()
                ) {
                // Four activity/vendor books grant entries by advancing this very counter. It is
                // therefore both their visibility source and their parent progress bar; publishing
                // the collected total is the native behavior, not the claim count used elsewhere.
                if (counterGranted) {
                    state->values[node.valueIndex] = collected;
                } else if (cumulative && collected > 0) {
                    // A normal cumulative book needs a collection counter distinct from a shared
                    // parent bar. Where its own slot is the bar, the extracted parent slot carries
                    // the collection value instead so the bar can continue counting claims.
                    std::uint16_t counterSlot = node.valueIndex;
                    if (parentSlot == static_cast<std::int32_t>(node.valueIndex)
                        && node.parentValueIndex != build_data::nodes::kUnavailableValueIndex) {
                        counterSlot = node.parentValueIndex;
                    }
                    if (static_cast<std::int32_t>(counterSlot)
                            < objective_slot_table::kRecordObjectiveRangeStart
                        && static_cast<std::size_t>(counterSlot) < state->values.size()) {
                        state->values[counterSlot] = collected;
                    }
                }

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
    const auto counterGrantedChapters = counter_granted_chapter_mask_locked();
    for (std::size_t index = 0; index < g_progress.size(); ++index) {
        if (g_progress[index] <= 0 || claimed_locked(static_cast<std::uint16_t>(index))
            || claimable_locked(static_cast<std::uint16_t>(index))) {
            continue;
        }
        const auto flagIndex = static_cast<std::uint16_t>(index);
        const auto found = std::lower_bound(
            table.begin(), table.end(), flagIndex,
            [](const objective_slot_table::RecordEntry& entry, std::uint16_t key) {
                return entry.flagIndex < key;
            });
        if (found == table.end() || found->flagIndex != flagIndex
            || found->objectiveCount != 1
            || static_cast<std::size_t>(found->firstObjective)
                   >= objective_slot_table::kObjectives.size()) {
            const ReservedObjective* reserved = find_reserved_objective(flagIndex);
            if (reserved == nullptr) {
                continue;
            }
            for (std::uint8_t slot = 0; slot < reserved->slotCount; ++slot) {
                const std::size_t valueSlot =
                    static_cast<std::size_t>(reserved->firstSlot) + slot;
                if (valueSlot < objectiveValues.size()) {
                    objectiveValues[valueSlot] =
                        std::min(g_progress[index], reserved->completionValue);
                    ++written;
                }
            }
            continue;
        }
        const auto& objective = objective_slot_table::kObjectives[found->firstObjective];
        if (static_cast<std::size_t>(objective.slot) >= objectiveValues.size()) {
            continue;
        }
        objectiveValues[objective.slot] = std::min(g_progress[index], objective.completionValue);
        ++written;
    }
    for (std::size_t word = 0; word < g_claimable.size(); ++word) {
        // Only claimable-and-unclaimed records. Writing claimed ones too was tried and redacted
        // nearly every lore book: record objective slots share the 2746-5686 range with the value
        // indices some book gates read (4619, 4719, 4991 among them), so writing a value per claim
        // trampled those gates. A claim already shows through its completion flag.
        std::uint64_t bits =
            g_claimable[word] & ~g_claimed[word] & ~counterGrantedChapters[word];
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
                const ReservedObjective* reserved = find_reserved_objective(flagIndex);
                if (reserved != nullptr) {
                    for (std::uint8_t slot = 0; slot < reserved->slotCount; ++slot) {
                        const std::size_t valueSlot =
                            static_cast<std::size_t>(reserved->firstSlot) + slot;
                        if (valueSlot < objectiveValues.size()) {
                            objectiveValues[valueSlot] = reserved->completionValue;
                            ++written;
                        }
                    }
                }
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
    NodeProgress progress{characterValues, 0};
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
    g_progress[flagIndex] = 0;
    g_claimable[static_cast<std::size_t>(flagIndex) / kWordBits] |=
        1ULL << (static_cast<std::size_t>(flagIndex) % kWordBits);
    // Written through immediately, as mark_claimed does: a pickup is the player's progress and has
    // to survive the process, not just the session that recorded it.
    store_claimable_locked();
    store_progress_locked();
    return true;
}

/** Advances one persisted objective and promotes it to claimable at its authored threshold. */
ObjectiveAdvance advance_single_objective(std::uint16_t flagIndex) noexcept {
    if (static_cast<std::size_t>(flagIndex) >= kIndexCapacity) {
        return ObjectiveAdvance::unavailable;
    }
    const auto found = std::lower_bound(
        objective_slot_table::kRecords.begin(), objective_slot_table::kRecords.end(), flagIndex,
        [](const objective_slot_table::RecordEntry& entry, std::uint16_t key) {
            return entry.flagIndex < key;
        });
    std::int32_t completion = 0;
    if (found != objective_slot_table::kRecords.end() && found->flagIndex == flagIndex
        && found->objectiveCount == 1
        && static_cast<std::size_t>(found->firstObjective)
               < objective_slot_table::kObjectives.size()) {
        completion = objective_slot_table::kObjectives[found->firstObjective].completionValue;
    } else {
        const ReservedObjective* reserved = find_reserved_objective(flagIndex);
        if (reserved == nullptr) {
            return ObjectiveAdvance::unavailable;
        }
        completion = reserved->completionValue;
    }
    if (completion <= 0) {
        return ObjectiveAdvance::unavailable;
    }

    const std::lock_guard<std::mutex> guard(g_lock);
    if (claimed_locked(flagIndex) || claimable_locked(flagIndex)) {
        return ObjectiveAdvance::alreadyHeld;
    }
    const std::int32_t next =
        g_progress[flagIndex] >= completion - 1 ? completion : g_progress[flagIndex] + 1;
    if (next >= completion) {
        g_progress[flagIndex] = 0;
        g_claimable[static_cast<std::size_t>(flagIndex) / kWordBits] |=
            std::uint64_t{1} << (static_cast<std::size_t>(flagIndex) % kWordBits);
        store_progress_locked();
        store_claimable_locked();
        return ObjectiveAdvance::completed;
    }
    g_progress[flagIndex] = next;
    store_progress_locked();
    return ObjectiveAdvance::advanced;
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
