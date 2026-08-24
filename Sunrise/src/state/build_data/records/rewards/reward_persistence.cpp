#include "reward_persistence.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <windows.h>

#include "../../../../core/logging/log.h"
#include "../../../../core/filesystem/path.h"
#include "../../runtime.h"
#include "reward_catalog.h"

namespace sunrise::state::build_data::records::rewards {
namespace {

/**
 * Shipped beside settings.json in the artifact directory, not under `\cache\`: the record and node
 * tables there are extraction output the running client rebuilds on a cold cache, while this file
 * is generated ahead of time by an external tool and is never written by this process.
 */
constexpr std::wstring_view kRewardFileSuffix = L"\\record_rewards.bin";
/** Identifies the file on sight, so an unrelated file of the right length cannot be read as one. */
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'R', 'W', 'D', '1'};

core::path::Buffer g_path{};
bool g_pathReady{};

/** Reports the one line this domain logs per load attempt, success or not. */
void report(const char* result,
           std::size_t rows,
           std::size_t records,
           std::size_t unresolvedItems) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=recrewards stage=load result=%s rows=%zu records=%zu "
                                      "unresolved_items=%zu",
                                      result,
                                      rows,
                                      records,
                                      unresolvedItems);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         std::string_view{result} == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Header carried ahead of the rows, in the same style as the record and node caches: the row
 * width is written out and checked on the way back in, so a shape change on either side rejects a
 * stale layout instead of misreading it.
 */
struct Header {
    std::array<char, 8> magic{};
    std::uint32_t rows{};
    std::uint32_t rowWidth{};
};

/** @return Distinct record hashes named across every row. */
[[nodiscard]] std::size_t distinct_record_count(std::vector<std::uint32_t> hashes) noexcept {
    std::sort(hashes.begin(), hashes.end());
    return static_cast<std::size_t>(std::unique(hashes.begin(), hashes.end()) - hashes.begin());
}

} // namespace

/** Derives the reward file path. It lives beside settings.json, not the extraction cache. */
bool initialize(void* module) noexcept {
    g_pathReady = false;
    if (!core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kRewardFileSuffix)) {
        report("path_fail", 0, 0, 0);
        return false;
    }
    g_pathReady = true;
    return true;
}

/**
 * Loads the shipped reward table and publishes it. A missing or empty file is a clean no-op: the
 * feature stays inert and the settings-authored table remains the operator's only reward source.
 */
bool load_and_publish() noexcept {
    if (!g_pathReady) {
        return false;
    }
    const HANDLE file = CreateFileW(
        g_path.chars.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // Not shipped for this deployment. Reported as an ordinary zero-row load, not a fault.
        report("ok", 0, 0, 0);
        return true;
    }

    Header header{};
    DWORD read = 0;
    const BOOL headerCall = ReadFile(file, &header, sizeof header, &read, nullptr);
    if (headerCall != FALSE && read == 0) {
        // A zero-byte file is an explicit "no generated rewards" deployment, not corruption.
        CloseHandle(file);
        report("ok", 0, 0, 0);
        return true;
    }
    const bool headerRead = headerCall != FALSE && read == sizeof header;
    if (!headerRead || std::memcmp(header.magic.data(), kMagic.data(), kMagic.size()) != 0
        || header.rowWidth != sizeof(RewardRow) || header.rows > kRewardCapacity) {
        CloseHandle(file);
        report(headerRead ? "rejected" : "header_fail", header.rows, 0, 0);
        return false;
    }
    if (header.rows == 0) {
        CloseHandle(file);
        report("ok", 0, 0, 0);
        return true;
    }

    std::vector<RewardRow> rows(header.rows);
    const auto expected = static_cast<DWORD>(rows.size() * sizeof(RewardRow));
    const bool rowsRead =
        ReadFile(file, rows.data(), expected, &read, nullptr) != FALSE && read == expected;
    CloseHandle(file);
    if (!rowsRead) {
        report("read_fail", rows.size(), 0, 0);
        return false;
    }
    if (!valid(std::span<const RewardRow>{rows}) || !replace(std::span<const RewardRow>{rows})) {
        report("publish_fail", rows.size(), 0, 0);
        return false;
    }

    // The three numbers this domain reports are computed once, here, rather than on every claim:
    // an item hash this build never installed (vaulted, or authored in a later era) is expected,
    // and a claim skipping it silently is cheaper than re-deriving the same count each time.
    std::vector<std::uint32_t> recordHashes;
    recordHashes.reserve(rows.size());
    std::size_t unresolved = 0;
    for (const RewardRow& row : rows) {
        recordHashes.push_back(row.recordHash);
        state::build_data::items::Definition item{};
        if (!state::build_data::find_item_definition_hash(row.itemHash, item)) {
            ++unresolved;
        }
    }
    report("ok", rows.size(), distinct_record_count(std::move(recordHashes)), unresolved);
    return true;
}

} // namespace sunrise::state::build_data::records::rewards
