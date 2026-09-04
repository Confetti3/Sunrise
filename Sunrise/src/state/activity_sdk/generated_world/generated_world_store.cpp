#include <algorithm>
#include <array>
#include <cwchar>

#include "../../build_data/scriptables/coverage.h"
#include "store.h"

namespace sunrise::state::activity_sdk::generated_world::store {
namespace {

namespace catalog = build_data::scriptables;

/** @return True when one digest carries an identity instead of the failure value. */
[[nodiscard]] bool nonzero(const Digest& value) noexcept {
    return value != Digest{};
}

/** Checks a fixed-width manifest name against one caller-owned exact name. */
[[nodiscard]] bool record_name_matches(const manifest::Record& record,
                                       std::string_view expected) noexcept {
    return record.scenarioNameLength == expected.size()
           && std::equal(expected.begin(), expected.end(), record.scenarioName.begin());
}

/** Maps the codec refusal without losing stale-source or missing-file detail. */
[[nodiscard]] RecordLoadStatus map_status(LoadStatus value) noexcept {
    switch (value) {
    case LoadStatus::loaded:
        return RecordLoadStatus::loaded;
    case LoadStatus::missing:
        return RecordLoadStatus::missing;
    case LoadStatus::scenarioMismatch:
        return RecordLoadStatus::scenarioMismatch;
    case LoadStatus::sourceMismatch:
        return RecordLoadStatus::sourceMismatch;
    case LoadStatus::versionMismatch:
    case LoadStatus::invalid:
        return RecordLoadStatus::invalid;
    }
    return RecordLoadStatus::invalid;
}

} // namespace

/** @return The stable log name of one record load status. */
const char* status_name(RecordLoadStatus value) noexcept {
    switch (value) {
    case RecordLoadStatus::loaded:
        return "loaded";
    case RecordLoadStatus::invalidIdentity:
        return "invalid_identity";
    case RecordLoadStatus::scenarioMismatch:
        return "scenario_mismatch";
    case RecordLoadStatus::missing:
        return "missing";
    case RecordLoadStatus::sourceMismatch:
        return "source_mismatch";
    case RecordLoadStatus::payloadMismatch:
        return "payload_mismatch";
    case RecordLoadStatus::invalid:
        return "invalid";
    }
    return "invalid";
}

const manifest::Record* find_record(std::span<const manifest::Record> records,
                                    std::uint32_t scenarioTag) noexcept {
    const auto found = std::lower_bound(
        records.begin(),
        records.end(),
        scenarioTag,
        [](const manifest::Record& row, std::uint32_t tag) { return row.scenarioTag < tag; });
    return found != records.end() && found->scenarioTag == scenarioTag ? &*found : nullptr;
}

/** Builds the on-disk path of one shard from its scenario tag and payload digest. */
bool shard_path(std::wstring_view scenarioDirectory,
                std::uint32_t scenarioTag,
                const Digest& payloadSha256,
                std::wstring& output) noexcept {
    static constexpr std::array<wchar_t, 16> kDigits{
        L'0',
        L'1',
        L'2',
        L'3',
        L'4',
        L'5',
        L'6',
        L'7',
        L'8',
        L'9',
        L'a',
        L'b',
        L'c',
        L'd',
        L'e',
        L'f',
    };
    output.clear();
    if (scenarioDirectory.empty() || scenarioTag == 0 || !nonzero(payloadSha256)) {
        return false;
    }
    std::array<wchar_t, 10> prefix{};
    const int length =
        std::swprintf(prefix.data(), prefix.size(), L"%08X-", static_cast<unsigned>(scenarioTag));
    if (length != 9) {
        return false;
    }
    try {
        output.assign(scenarioDirectory);
        if (output.back() != L'\\' && output.back() != L'/') {
            output.push_back(L'\\');
        }
        output.append(prefix.data(), static_cast<std::size_t>(length));
        for (const std::byte byte : payloadSha256) {
            const unsigned value = std::to_integer<unsigned>(byte);
            output.push_back(kDigits[(value >> 4U) & 0xFU]);
            output.push_back(kDigits[value & 0xFU]);
        }
        output.append(L".pack");
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Loads and authenticates one manifest record's shard. @param status Receives the outcome. */
bool load_record(std::wstring_view scenarioDirectory,
                 std::uint32_t expectedScenarioTag,
                 std::string_view expectedScenarioName,
                 const Digest& expectedSourceFingerprint,
                 const manifest::Record& record,
                 std::shared_ptr<const catalog::Snapshot>& output,
                 RecordLoadStatus& status) noexcept {
    output.reset();
    status = RecordLoadStatus::invalid;
    if (expectedScenarioTag == 0 || expectedScenarioName.empty()
        || expectedScenarioName.size() >= catalog::kScenarioNameCapacity
        || !nonzero(expectedSourceFingerprint) || !nonzero(record.shardPayloadSha256)) {
        status = RecordLoadStatus::invalidIdentity;
        return false;
    }
    if (record.scenarioTag != expectedScenarioTag
        || !record_name_matches(record, expectedScenarioName)) {
        status = RecordLoadStatus::scenarioMismatch;
        return false;
    }

    std::wstring path;
    if (!shard_path(scenarioDirectory, expectedScenarioTag, record.shardPayloadSha256, path)) {
        status = RecordLoadStatus::invalidIdentity;
        return false;
    }
    std::shared_ptr<catalog::Snapshot> pending;
    try {
        pending = std::make_shared<catalog::Snapshot>();
    } catch (...) {
        return false;
    }
    Digest payload{};
    LoadStatus loadStatus = LoadStatus::invalid;
    if (!generated_world::load(path.c_str(),
                               expectedScenarioTag,
                               expectedSourceFingerprint,
                               *pending,
                               payload,
                               loadStatus)) {
        status = map_status(loadStatus);
        return false;
    }
    if (payload != record.shardPayloadSha256) {
        status = RecordLoadStatus::payloadMismatch;
        return false;
    }
    if (pending->scenarioTag != expectedScenarioTag
        || pending->scenarioNameLength != expectedScenarioName.size()
        || !std::equal(expectedScenarioName.begin(),
                       expectedScenarioName.end(),
                       pending->scenarioName.begin())) {
        status = RecordLoadStatus::scenarioMismatch;
        return false;
    }
    // decode_payload only accepts full-coverage snapshots, so coverage needs no second check.
    output = std::move(pending);
    status = RecordLoadStatus::loaded;
    return true;
}

} // namespace sunrise::state::activity_sdk::generated_world::store
