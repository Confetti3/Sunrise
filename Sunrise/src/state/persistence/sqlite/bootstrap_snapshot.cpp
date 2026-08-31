#include "bootstrap_snapshot.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "../../activity/defaults/activity_defaults_validation.h"
#include "../../entitlements/validation.h"

namespace sunrise::state::persistence::sqlite {
namespace {

enum class FlagScope : int { account, profile, character, characterObject };
enum class ValueScope : int { account, characterObject };
enum class ProgressionScope : int { account, character };

[[nodiscard]] bool done(sqlite3_stmt* statement) noexcept {
    return statement != nullptr && sqlite3_step(statement) == SQLITE_DONE;
}

[[nodiscard]] bool integer(sqlite3_stmt* statement, int column, sqlite3_int64& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
        return false;
    }
    output = sqlite3_column_int64(statement, column);
    return true;
}

[[nodiscard]] bool text(sqlite3_stmt* statement,
                        int column,
                        std::string_view& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        return false;
    }
    const int size = sqlite3_column_bytes(statement, column);
    const unsigned char* value = sqlite3_column_text(statement, column);
    if (size < 0 || (size != 0 && value == nullptr)) {
        return false;
    }
    output = {reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
    return true;
}

[[nodiscard]] bool bind_text(sqlite3_stmt* statement,
                             int index,
                             std::string_view value) noexcept {
    return statement != nullptr
           && value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
           && sqlite3_bind_text(statement,
                                index,
                                value.data(),
                                static_cast<int>(value.size()),
                                SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] bool hexadecimal(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char byte) noexcept {
        return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F')
               || (byte >= 'a' && byte <= 'f');
    });
}

template <std::size_t Capacity>
[[nodiscard]] bool copy_text(std::string_view value,
                             std::array<char, Capacity>& output,
                             std::size_t& length) noexcept {
    if (value.size() > output.size()) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.begin());
    length = value.size();
    return true;
}

[[nodiscard]] bool valid_uuid(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (value.size() != kDatabaseUuidCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if ((separator && value[index] != '-')
            || (!separator && !hexadecimal(value.substr(index, 1)))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool load_metadata_row(Database& database, StoreMetadata& output) noexcept {
    Statement query(database.get(),
                    "SELECT singleton,database_uuid,seed_revision,seed_hash,content_build,"
                    "image_timestamp,image_size,configured_equipment_hash,legacy_source_version,"
                    "legacy_source_hash,account_origin,"
                    "bootstrap_origin,compaction_state FROM store_metadata;");
    if (query.get() == nullptr || sqlite3_step(query.get()) != SQLITE_ROW) {
        return false;
    }
    sqlite3_int64 singleton = 0;
    sqlite3_int64 seedRevision = -1;
    sqlite3_int64 contentBuild = -1;
    sqlite3_int64 legacyVersion = -1;
    sqlite3_int64 accountOrigin = -1;
    sqlite3_int64 bootstrapOrigin = -1;
    sqlite3_int64 compactionState = -1;
    std::string_view uuid;
    std::string_view seedHash;
    std::string_view legacyHash;
    if (!integer(query.get(), 0, singleton) || singleton != 1 || !text(query.get(), 1, uuid)
        || !valid_uuid(uuid) || !copy_text(uuid, output.databaseUuid, output.databaseUuidLength)
        || !integer(query.get(), 2, seedRevision) || seedRevision < 0
        || seedRevision > (std::numeric_limits<std::uint32_t>::max)()
        || !text(query.get(), 3, seedHash) || seedHash.size() != kSha256TextCapacity
        || !hexadecimal(seedHash) || !integer(query.get(), 4, contentBuild) || contentBuild < 0
        || contentBuild > (std::numeric_limits<std::uint32_t>::max)()
        || !column_u32(query.get(), 5, output.buildIdentity.imageTimestamp)
        || !column_u32(query.get(), 6, output.buildIdentity.imageSize)
        || !column_u64(query.get(), 7, output.buildIdentity.configuredEquipmentHash)
        || !integer(query.get(), 8, legacyVersion) || legacyVersion < 0
        || legacyVersion > (std::numeric_limits<std::uint32_t>::max)()
        || !text(query.get(), 9, legacyHash) || legacyHash.size() != kSha256TextCapacity
        || !hexadecimal(legacyHash) || !integer(query.get(), 10, accountOrigin)
        || accountOrigin < 0 || accountOrigin > 2
        || !integer(query.get(), 11, bootstrapOrigin) || bootstrapOrigin < 0
        || bootstrapOrigin > 2 || !integer(query.get(), 12, compactionState)
        || compactionState < 0 || compactionState > 2
        || sqlite3_step(query.get()) != SQLITE_DONE) {
        return false;
    }
    output.seedRevision = static_cast<std::uint32_t>(seedRevision);
    std::copy(seedHash.begin(), seedHash.end(), output.seedHash.begin());
    output.contentBuild = static_cast<std::uint32_t>(contentBuild);
    output.legacySourceVersion = static_cast<std::uint32_t>(legacyVersion);
    std::copy(legacyHash.begin(), legacyHash.end(), output.legacySourceHash.begin());
    output.accountOrigin = static_cast<std::uint8_t>(accountOrigin);
    output.bootstrapOrigin = static_cast<std::uint8_t>(bootstrapOrigin);
    output.compactionState = static_cast<std::uint8_t>(compactionState);
    return true;
}

[[nodiscard]] std::span<std::uint8_t> flag_bank(unlocks::Table& table, int scope) noexcept {
    switch (static_cast<FlagScope>(scope)) {
        case FlagScope::account: return table.accountFlags;
        case FlagScope::profile: return table.profileFlags;
        case FlagScope::character: return table.characterFlags;
        case FlagScope::characterObject: return table.characterObjectFlags;
    }
    return {};
}

[[nodiscard]] std::span<const std::uint8_t> flag_bank(const unlocks::Table& table,
                                                       int scope) noexcept {
    switch (static_cast<FlagScope>(scope)) {
        case FlagScope::account: return table.accountFlags;
        case FlagScope::profile: return table.profileFlags;
        case FlagScope::character: return table.characterFlags;
        case FlagScope::characterObject: return table.characterObjectFlags;
    }
    return {};
}

[[nodiscard]] std::span<std::int32_t> value_bank(unlocks::Table& table, int scope) noexcept {
    return static_cast<ValueScope>(scope) == ValueScope::account
               ? std::span<std::int32_t>(table.objectiveValues)
               : std::span<std::int32_t>(table.characterObjectValues);
}

[[nodiscard]] std::span<const std::int32_t> value_bank(const unlocks::Table& table,
                                                        int scope) noexcept {
    return static_cast<ValueScope>(scope) == ValueScope::account
               ? std::span<const std::int32_t>(table.objectiveValues)
               : std::span<const std::int32_t>(table.characterObjectValues);
}

[[nodiscard]] unlocks::ProgressionBank& progression_bank(unlocks::Table& table,
                                                          int scope) noexcept {
    return static_cast<ProgressionScope>(scope) == ProgressionScope::account
               ? table.accountProgressions
               : table.characterProgressions;
}

[[nodiscard]] const unlocks::ProgressionBank& progression_bank(const unlocks::Table& table,
                                                                int scope) noexcept {
    return static_cast<ProgressionScope>(scope) == ProgressionScope::account
               ? table.accountProgressions
               : table.characterProgressions;
}

[[nodiscard]] bool load_entitlements(Database& database, entitlements::Table& output) noexcept {
    Statement query(database.get(),
                    "SELECT position,name,ownership FROM entitlements ORDER BY position;");
    std::size_t expected = 0;
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        sqlite3_int64 position = -1;
        sqlite3_int64 ownership = -1;
        std::string_view name;
        if (expected >= output.entries.size() || !integer(query.get(), 0, position)
            || position != static_cast<sqlite3_int64>(expected) || !text(query.get(), 1, name)
            || name.empty() || name.size() >= entitlements::kNameCapacity
            || !integer(query.get(), 2, ownership) || ownership < 0 || ownership > 2) {
            return false;
        }
        auto& entry = output.entries[expected++];
        std::copy(name.begin(), name.end(), entry.name.begin());
        entry.nameLength = static_cast<std::uint8_t>(name.size());
        entry.ownership = static_cast<entitlements::Ownership>(ownership);
    }
    output.count = expected;
    return query.get() != nullptr && result == SQLITE_DONE && entitlements::valid(output);
}

[[nodiscard]] bool load_flag_runs(Database& database, unlocks::Table& output) noexcept {
    Statement query(database.get(),
                    "SELECT scope,position,start_index,run_length FROM unlock_flag_runs "
                    "ORDER BY scope,position;");
    std::array<std::size_t, 4> positions{};
    std::array<std::size_t, 4> previousEnds{};
    std::array<bool, 4> seen{};
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        sqlite3_int64 scope = -1;
        sqlite3_int64 position = -1;
        sqlite3_int64 start = -1;
        sqlite3_int64 length = -1;
        if (!integer(query.get(), 0, scope) || scope < 0 || scope >= 4
            || !integer(query.get(), 1, position)
            || position != static_cast<sqlite3_int64>(positions[scope]++)
            || !integer(query.get(), 2, start) || start < 0
            || !integer(query.get(), 3, length) || length <= 0) {
            return false;
        }
        std::span<std::uint8_t> bank = flag_bank(output, static_cast<int>(scope));
        const auto first = static_cast<std::uint64_t>(start);
        const auto count = static_cast<std::uint64_t>(length);
        if (first >= bank.size() || count > bank.size() - first
            || (seen[scope] && first <= previousEnds[scope])) {
            return false;
        }
        std::fill_n(bank.begin() + static_cast<std::size_t>(first),
                    static_cast<std::size_t>(count),
                    unlocks::kFlagSet);
        seen[scope] = true;
        previousEnds[scope] = static_cast<std::size_t>(first + count);
    }
    return query.get() != nullptr && result == SQLITE_DONE;
}

[[nodiscard]] bool load_values(Database& database, unlocks::Table& output) noexcept {
    Statement query(database.get(),
                    "SELECT scope,position,slot,value FROM unlock_objective_values "
                    "ORDER BY scope,position;");
    std::array<std::size_t, 2> positions{};
    std::array<std::size_t, 2> previousSlots{};
    std::array<bool, 2> seen{};
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        sqlite3_int64 scope = -1;
        sqlite3_int64 position = -1;
        sqlite3_int64 slot = -1;
        std::int32_t value = 0;
        if (!integer(query.get(), 0, scope) || scope < 0 || scope >= 2
            || !integer(query.get(), 1, position)
            || position != static_cast<sqlite3_int64>(positions[scope]++)
            || !integer(query.get(), 2, slot) || slot < 0
            || !column_i32(query.get(), 3, value) || value == 0) {
            return false;
        }
        std::span<std::int32_t> bank = value_bank(output, static_cast<int>(scope));
        if (static_cast<std::uint64_t>(slot) >= bank.size()
            || (seen[scope] && static_cast<std::size_t>(slot) <= previousSlots[scope])) {
            return false;
        }
        bank[static_cast<std::size_t>(slot)] = value;
        seen[scope] = true;
        previousSlots[scope] = static_cast<std::size_t>(slot);
    }
    return query.get() != nullptr && result == SQLITE_DONE;
}

[[nodiscard]] bool load_progressions(Database& database, unlocks::Table& output) noexcept {
    Statement query(database.get(),
                    "SELECT scope,position,definition_index,lane0,lane1,lane2 "
                    "FROM unlock_progressions ORDER BY scope,position;");
    std::array<std::size_t, 2> positions{};
    std::array<std::size_t, 2> previousDefinitions{};
    std::array<bool, 2> seen{};
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(query.get())) == SQLITE_ROW) {
        sqlite3_int64 scope = -1;
        sqlite3_int64 position = -1;
        sqlite3_int64 definition = -1;
        unlocks::ProgressionLanes lanes{};
        if (!integer(query.get(), 0, scope) || scope < 0 || scope >= 2
            || !integer(query.get(), 1, position)
            || position != static_cast<sqlite3_int64>(positions[scope]++)
            || !integer(query.get(), 2, definition) || definition < 0
            || !column_i32(query.get(), 3, lanes[0]) || !column_i32(query.get(), 4, lanes[1])
            || !column_i32(query.get(), 5, lanes[2])
            || std::all_of(lanes.begin(), lanes.end(), [](std::int32_t value) { return value == 0; })) {
            return false;
        }
        auto& bank = progression_bank(output, static_cast<int>(scope));
        if (static_cast<std::uint64_t>(definition) >= bank.size()
            || (seen[scope]
                && static_cast<std::size_t>(definition) <= previousDefinitions[scope])) {
            return false;
        }
        bank[static_cast<std::size_t>(definition)] = lanes;
        seen[scope] = true;
        previousDefinitions[scope] = static_cast<std::size_t>(definition);
    }
    return query.get() != nullptr && result == SQLITE_DONE;
}

[[nodiscard]] bool load_family5(Database& database, Family5State& output) noexcept {
    Statement flags(database.get(),
                    "SELECT position,slot,value FROM family5_flag_overrides ORDER BY position;");
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(flags.get())) == SQLITE_ROW) {
        sqlite3_int64 position = -1;
        sqlite3_int64 slot = -1;
        sqlite3_int64 value = -1;
        if (output.flagCount >= output.flags.size() || !integer(flags.get(), 0, position)
            || position != static_cast<sqlite3_int64>(output.flagCount)
            || !integer(flags.get(), 1, slot) || slot < 0 || slot > 23499
            || !integer(flags.get(), 2, value) || value < 0 || value > 2) {
            return false;
        }
        output.flags[output.flagCount++] = {
            static_cast<std::uint16_t>(slot), static_cast<std::uint8_t>(value)};
    }
    if (flags.get() == nullptr || result != SQLITE_DONE) {
        return false;
    }
    Statement values(database.get(),
                     "SELECT position,slot,value FROM family5_value_overrides ORDER BY position;");
    while ((result = sqlite3_step(values.get())) == SQLITE_ROW) {
        sqlite3_int64 position = -1;
        sqlite3_int64 slot = -1;
        std::int32_t value = 0;
        if (output.valueCount >= output.values.size() || !integer(values.get(), 0, position)
            || position != static_cast<sqlite3_int64>(output.valueCount)
            || !integer(values.get(), 1, slot) || slot < 0 || slot > 15499
            || !column_i32(values.get(), 2, value)) {
            return false;
        }
        output.values[output.valueCount++] = {static_cast<std::uint16_t>(slot), value};
    }
    return values.get() != nullptr && result == SQLITE_DONE;
}

[[nodiscard]] bool copy_package(std::string_view text,
                                std::array<std::int8_t,
                                           activity::destination::kPackageNameCapacity>& output,
                                std::uint8_t& length) noexcept {
    if (text.empty() || text.size() > output.size()) {
        return false;
    }
    std::copy(text.begin(), text.end(), output.begin());
    length = static_cast<std::uint8_t>(text.size());
    return true;
}

[[nodiscard]] bool load_activity(Database& database,
                                 activity::defaults::ActivityDefaults& output) noexcept {
    Statement defaults(database.get(),
                       "SELECT singleton,reason,previous_activity_index,activity_index,package_name,"
                       "bubble_count,stateful_bubble_mask,initial_slice_set,spawn_set_hash,"
                       "roster_key_from_identity,roster_key_on_all_slots FROM activity_defaults;");
    if (defaults.get() == nullptr || sqlite3_step(defaults.get()) != SQLITE_ROW) {
        return false;
    }
    sqlite3_int64 singleton = 0;
    sqlite3_int64 reason = 0;
    sqlite3_int64 previous = 0;
    sqlite3_int64 activityIndex = 0;
    sqlite3_int64 bubbleCount = 0;
    sqlite3_int64 initialSlice = 0;
    std::string_view package;
    auto& destination = output.defaultDestination;
    if (!integer(defaults.get(), 0, singleton) || singleton != 1
        || !integer(defaults.get(), 1, reason) || reason < -1 || reason > 14
        || !integer(defaults.get(), 2, previous) || previous < -1 || previous > 4094
        || !integer(defaults.get(), 3, activityIndex) || activityIndex < 0
        || activityIndex > 4094 || !text(defaults.get(), 4, package)
        || !copy_package(package,
                         destination.selection.packageName,
                         destination.selection.packageNameLength)
        || !integer(defaults.get(), 5, bubbleCount) || bubbleCount < 1 || bubbleCount > 64
        || !column_u64(defaults.get(), 6, destination.fallback.statefulBubbleMask)
        || !integer(defaults.get(), 7, initialSlice) || initialSlice < 0 || initialSlice > 511
        || !column_u32(defaults.get(), 8, destination.fallback.spawnSetHash)
        || !column_bool(defaults.get(), 9, output.rosterKeyFromIdentity)
        || !column_bool(defaults.get(), 10, output.rosterKeyOnAllSlots)
        || sqlite3_step(defaults.get()) != SQLITE_DONE) {
        return false;
    }
    destination.selection.reason = static_cast<std::int8_t>(reason);
    destination.selection.previousActivityIndex = static_cast<std::int16_t>(previous);
    destination.selection.activityIndex = static_cast<std::int16_t>(activityIndex);
    destination.fallback.bubbleCount = static_cast<std::uint8_t>(bubbleCount);
    destination.fallback.initialSliceSet = static_cast<std::uint16_t>(initialSlice);

    Statement arrivals(database.get(),
                       "SELECT position,package_name,bubble,spawn_set_hash "
                       "FROM activity_arrival_overrides ORDER BY position;");
    int result = SQLITE_ERROR;
    while ((result = sqlite3_step(arrivals.get())) == SQLITE_ROW) {
        if (output.arrivalOverrideCount >= output.arrivalOverrides.size()) {
            return false;
        }
        sqlite3_int64 position = -1;
        std::string_view name;
        auto& row = output.arrivalOverrides[output.arrivalOverrideCount];
        if (!integer(arrivals.get(), 0, position)
            || position != output.arrivalOverrideCount || !text(arrivals.get(), 1, name)
            || name.empty() || name.size() > row.name.size()) {
            return false;
        }
        std::copy(name.begin(), name.end(), row.name.begin());
        row.nameLength = static_cast<std::uint8_t>(name.size());
        if (sqlite3_column_type(arrivals.get(), 2) != SQLITE_NULL) {
            sqlite3_int64 bubble = -1;
            if (!integer(arrivals.get(), 2, bubble) || bubble < 0 || bubble > 63) {
                return false;
            }
            row.bubble = static_cast<std::uint8_t>(bubble);
            row.hasBubble = true;
        }
        if (sqlite3_column_type(arrivals.get(), 3) != SQLITE_NULL) {
            if (!column_u32(arrivals.get(), 3, row.spawnSetHash)) {
                return false;
            }
            row.hasSpawnSetHash = true;
        }
        if (!row.hasBubble && !row.hasSpawnSetHash) {
            return false;
        }
        ++output.arrivalOverrideCount;
    }
    return arrivals.get() != nullptr && result == SQLITE_DONE
           && activity::defaults::valid(output);
}

[[nodiscard]] bool validate_snapshot(const BootstrapSnapshot& snapshot) noexcept {
    if (!entitlements::valid(snapshot.entitlements)
        || !activity::defaults::valid(snapshot.activityDefaults)
        || snapshot.family5.flagCount > snapshot.family5.flags.size()
        || snapshot.family5.valueCount > snapshot.family5.values.size()
        || snapshot.activityDefaults.arrivalOverrideCount
               > snapshot.activityDefaults.arrivalOverrides.size()) {
        return false;
    }
    for (int scope = 0; scope < 4; ++scope) {
        for (std::uint8_t value : flag_bank(snapshot.unlocks, scope)) {
            if (value != unlocks::kFlagClear && value != unlocks::kFlagSet) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < snapshot.family5.flagCount; ++index) {
        const auto& row = snapshot.family5.flags[index];
        if (row.slot > 23499 || row.value > 2) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.family5.valueCount; ++index) {
        if (snapshot.family5.values[index].slot > 15499) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool clear_bootstrap(Database& database) noexcept {
    return database.execute("DELETE FROM entitlements; DELETE FROM unlock_flag_runs; "
                            "DELETE FROM unlock_objective_values; DELETE FROM unlock_progressions; "
                            "DELETE FROM family5_flag_overrides; DELETE FROM family5_value_overrides; "
                            "DELETE FROM activity_arrival_overrides; DELETE FROM activity_defaults;");
}

[[nodiscard]] bool write_entitlements(Database& database,
                                      const entitlements::Table& table) noexcept {
    Statement insert(database.get(),
                     "INSERT INTO entitlements(position,name,ownership) VALUES(?,?,?);");
    for (std::size_t position = 0; position < table.count; ++position) {
        const auto& row = table.entries[position];
        if (!bind_count(insert.get(), 1, position)
            || !bind_text(insert.get(), 2, entitlements::name_of(row))
            || sqlite3_bind_int(insert.get(), 3, static_cast<int>(row.ownership)) != SQLITE_OK
            || !done(insert.get()) || !insert.reset()) {
            return false;
        }
    }
    return insert.get() != nullptr;
}

[[nodiscard]] bool write_unlocks(Database& database, const unlocks::Table& table) noexcept {
    Statement runs(database.get(),
                   "INSERT INTO unlock_flag_runs(scope,position,start_index,run_length) "
                   "VALUES(?,?,?,?);");
    for (int scope = 0; scope < 4; ++scope) {
        std::span<const std::uint8_t> bank = flag_bank(table, scope);
        std::size_t position = 0;
        for (std::size_t start = 0; start < bank.size();) {
            if (bank[start] == unlocks::kFlagClear) {
                ++start;
                continue;
            }
            std::size_t end = start + 1;
            while (end < bank.size() && bank[end] == unlocks::kFlagSet) {
                ++end;
            }
            if (sqlite3_bind_int(runs.get(), 1, scope) != SQLITE_OK
                || !bind_count(runs.get(), 2, position++) || !bind_count(runs.get(), 3, start)
                || !bind_count(runs.get(), 4, end - start) || !done(runs.get())
                || !runs.reset()) {
                return false;
            }
            start = end;
        }
    }

    Statement values(database.get(),
                     "INSERT INTO unlock_objective_values(scope,position,slot,value) "
                     "VALUES(?,?,?,?);");
    for (int scope = 0; scope < 2; ++scope) {
        std::size_t position = 0;
        const auto bank = value_bank(table, scope);
        for (std::size_t slot = 0; slot < bank.size(); ++slot) {
            if (bank[slot] == 0) {
                continue;
            }
            if (sqlite3_bind_int(values.get(), 1, scope) != SQLITE_OK
                || !bind_count(values.get(), 2, position++) || !bind_count(values.get(), 3, slot)
                || !bind_i32(values.get(), 4, bank[slot]) || !done(values.get())
                || !values.reset()) {
                return false;
            }
        }
    }

    Statement progressions(database.get(),
                           "INSERT INTO unlock_progressions(scope,position,definition_index,"
                           "lane0,lane1,lane2) VALUES(?,?,?,?,?,?);");
    for (int scope = 0; scope < 2; ++scope) {
        std::size_t position = 0;
        const auto& bank = progression_bank(table, scope);
        for (std::size_t definition = 0; definition < bank.size(); ++definition) {
            const auto& row = bank[definition];
            if (std::all_of(row.begin(), row.end(), [](std::int32_t value) { return value == 0; })) {
                continue;
            }
            if (sqlite3_bind_int(progressions.get(), 1, scope) != SQLITE_OK
                || !bind_count(progressions.get(), 2, position++)
                || !bind_count(progressions.get(), 3, definition)
                || !bind_i32(progressions.get(), 4, row[0])
                || !bind_i32(progressions.get(), 5, row[1])
                || !bind_i32(progressions.get(), 6, row[2]) || !done(progressions.get())
                || !progressions.reset()) {
                return false;
            }
        }
    }
    return runs.get() != nullptr && values.get() != nullptr && progressions.get() != nullptr;
}

[[nodiscard]] bool write_family5(Database& database, const Family5State& family5) noexcept {
    Statement flags(database.get(),
                    "INSERT INTO family5_flag_overrides(position,slot,value) VALUES(?,?,?);");
    for (std::size_t position = 0; position < family5.flagCount; ++position) {
        const auto& row = family5.flags[position];
        if (!bind_count(flags.get(), 1, position) || !bind_count(flags.get(), 2, row.slot)
            || sqlite3_bind_int(flags.get(), 3, row.value) != SQLITE_OK || !done(flags.get())
            || !flags.reset()) {
            return false;
        }
    }
    Statement values(database.get(),
                     "INSERT INTO family5_value_overrides(position,slot,value) VALUES(?,?,?);");
    for (std::size_t position = 0; position < family5.valueCount; ++position) {
        const auto& row = family5.values[position];
        if (!bind_count(values.get(), 1, position) || !bind_count(values.get(), 2, row.slot)
            || !bind_i32(values.get(), 3, row.value) || !done(values.get()) || !values.reset()) {
            return false;
        }
    }
    return flags.get() != nullptr && values.get() != nullptr;
}

[[nodiscard]] bool write_activity(Database& database,
                                  const activity::defaults::ActivityDefaults& activity) noexcept {
    const auto& destination = activity.defaultDestination;
    const auto& selection = destination.selection;
    const auto& fallback = destination.fallback;
    Statement defaults(database.get(),
                       "INSERT INTO activity_defaults VALUES(1,?,?,?,?,?,?,?,?,?,?);");
    const std::string_view package(reinterpret_cast<const char*>(selection.packageName.data()),
                                   selection.packageNameLength);
    if (sqlite3_bind_int(defaults.get(), 1, selection.reason) != SQLITE_OK
        || sqlite3_bind_int(defaults.get(), 2, selection.previousActivityIndex) != SQLITE_OK
        || sqlite3_bind_int(defaults.get(), 3, selection.activityIndex) != SQLITE_OK
        || !bind_text(defaults.get(), 4, package)
        || sqlite3_bind_int(defaults.get(), 5, fallback.bubbleCount) != SQLITE_OK
        || !bind_u64(defaults.get(), 6, fallback.statefulBubbleMask)
        || sqlite3_bind_int(defaults.get(), 7, fallback.initialSliceSet) != SQLITE_OK
        || !bind_u32(defaults.get(), 8, fallback.spawnSetHash)
        || !bind_bool(defaults.get(), 9, activity.rosterKeyFromIdentity)
        || !bind_bool(defaults.get(), 10, activity.rosterKeyOnAllSlots) || !done(defaults.get())) {
        return false;
    }

    Statement arrivals(database.get(),
                       "INSERT INTO activity_arrival_overrides(position,package_name,bubble,"
                       "spawn_set_hash) VALUES(?,?,?,?);");
    for (std::size_t position = 0; position < activity.arrivalOverrideCount; ++position) {
        const auto& row = activity.arrivalOverrides[position];
        const std::string_view name(row.name.data(), row.nameLength);
        if (name.empty() || name.size() > row.name.size()
            || (!row.hasBubble && !row.hasSpawnSetHash)
            || !bind_count(arrivals.get(), 1, position) || !bind_text(arrivals.get(), 2, name)
            || (row.hasBubble ? sqlite3_bind_int(arrivals.get(), 3, row.bubble)
                              : sqlite3_bind_null(arrivals.get(), 3)) != SQLITE_OK
            || (row.hasSpawnSetHash ? sqlite3_bind_int64(arrivals.get(), 4, row.spawnSetHash)
                                    : sqlite3_bind_null(arrivals.get(), 4)) != SQLITE_OK
            || !done(arrivals.get()) || !arrivals.reset()) {
            return false;
        }
    }
    return arrivals.get() != nullptr;
}

} // namespace

bool load_store_metadata(Database& database, StoreMetadata& output) noexcept {
    output = {};
    if (!database.healthy() || database.inspect_schema() != OpenResult::opened
        || database.schema_version() != kSchemaVersion) {
        return false;
    }
    StoreMetadata loaded{};
    if (!load_metadata_row(database, loaded)) {
        return false;
    }
    output = loaded;
    return true;
}

bool load_bootstrap(Database& database, BootstrapSnapshot& output) noexcept {
    output = {};
    StoreMetadata metadata{};
    if (!database.healthy() || database.inspect_schema() != OpenResult::opened
        || database.schema_version() != kSchemaVersion
        || !load_metadata_row(database, metadata)) {
        return false;
    }
    BootstrapSnapshot loaded{};
    if (!load_entitlements(database, loaded.entitlements)
        || !load_flag_runs(database, loaded.unlocks) || !load_values(database, loaded.unlocks)
        || !load_progressions(database, loaded.unlocks) || !load_family5(database, loaded.family5)
        || !load_activity(database, loaded.activityDefaults)) {
        return false;
    }
    output = loaded;
    return true;
}

bool save_bootstrap(Database& database, const BootstrapSnapshot& snapshot) noexcept {
    StoreMetadata metadata{};
    if (!database.healthy() || database.inspect_schema() != OpenResult::opened
        || !database.is_writable() || database.schema_version() != kSchemaVersion
        || !load_metadata_row(database, metadata) || !validate_snapshot(snapshot)) {
        return false;
    }
    bool committed = false;
    {
        Transaction transaction(database);
        if (transaction.active() && clear_bootstrap(database)
            && write_entitlements(database, snapshot.entitlements)
            && write_unlocks(database, snapshot.unlocks)
            && write_family5(database, snapshot.family5)
            && write_activity(database, snapshot.activityDefaults)) {
            committed = transaction.commit();
        } else if (transaction.active()) {
            (void)transaction.rollback();
        }
    }
    if (!committed && !database.healthy()) {
        (void)database.close_unhealthy();
    }
    return committed;
}

} // namespace sunrise::state::persistence::sqlite
