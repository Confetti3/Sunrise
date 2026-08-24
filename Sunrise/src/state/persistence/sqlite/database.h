#pragma once

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "../../../core/filesystem/path.h"

#if defined(_MSC_VER)
#pragma comment(lib, "winsqlite3.lib")
#endif

namespace sunrise::state::persistence::sqlite::detail {

/** Version of the relational SQLite schema owned by this implementation. */
inline constexpr int kSchemaVersion = 1;
/** Version of the account row contract independent of SQLite table migrations. */
inline constexpr int kAccountFormatVersion = 1;
/** Version of the explicit account-settings payload stored in the account row. */
inline constexpr std::uint32_t kSettingsPayloadVersion = 1;
/** Fixed payload storage is comfortably above the current settings and binding table. */
inline constexpr std::size_t kSettingsPayloadCapacity = 1024;
/** Wait long enough for a prior checkpoint to release the single-writer lock. */
inline constexpr int kBusyTimeoutMilliseconds = 3'000;
/** Stable discriminator values stored in character_items.location. */
inline constexpr int kEquipmentItemLocation = 0;
inline constexpr int kInventoryItemLocation = 1;
/** SQLite database stored in Sunrise's generated-artifact directory. */
inline constexpr std::wstring_view kDatabaseSuffix = L"\\state.sqlite3";

/** Outcome of opening and migrating the database. */
enum class OpenResult {
    opened,
    newer,
    failed,
};

/** Owns one prepared SQLite statement. */
class Statement final {
public:
    Statement(sqlite3* database, const char* sql) noexcept {
        if (database != nullptr && sql != nullptr) {
            (void)sqlite3_prepare_v2(database, sql, -1, &value_, nullptr);
        }
    }

    ~Statement() noexcept {
        if (value_ != nullptr) {
            (void)sqlite3_finalize(value_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    /** @return Prepared statement handle, or null when preparation failed. */
    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return value_;
    }

    /** Resets one reusable insert and clears every prior binding. */
    [[nodiscard]] bool reset() noexcept {
        return value_ != nullptr && sqlite3_reset(value_) == SQLITE_OK
               && sqlite3_clear_bindings(value_) == SQLITE_OK;
    }

private:
    sqlite3_stmt* value_{};
};

/** Owns one SQLite connection. */
class Database final {
public:
    ~Database() noexcept {
        if (value_ != nullptr) {
            (void)sqlite3_close(value_);
        }
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database() = default;

    /** Opens the database beside Sunrise's other generated artifacts. */
    [[nodiscard]] bool open(void* module) noexcept {
        core::path::Buffer path;
        if (module == nullptr || !core::path::artifact_directory(module, path)
            || !core::path::append(path, kDatabaseSuffix)) {
            return false;
        }
        if (sqlite3_open16(path.chars.data(), &value_) != SQLITE_OK || value_ == nullptr) {
            if (value_ != nullptr) {
                (void)sqlite3_close(value_);
                value_ = nullptr;
            }
            return false;
        }
        (void)sqlite3_extended_result_codes(value_, 1);
        return sqlite3_busy_timeout(value_, kBusyTimeoutMilliseconds) == SQLITE_OK;
    }

    /** Enables write behavior only after this build accepts the on-disk schema. */
    [[nodiscard]] bool configure_supported_schema() noexcept {
        if (!execute("PRAGMA foreign_keys = ON;")
            || !execute("PRAGMA synchronous = FULL;")) {
            return false;
        }
        // WAL makes later write-through checkpoints practical. The fallback journal remains safe
        // when the host or filesystem refuses the mode transition.
        (void)execute("PRAGMA journal_mode = WAL;");
        return true;
    }

    /** Executes one or more SQL statements without returning rows. */
    [[nodiscard]] bool execute(const char* sql) noexcept {
        return value_ != nullptr && sql != nullptr
               && sqlite3_exec(value_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    /** @return Borrowed connection handle. */
    [[nodiscard]] sqlite3* get() const noexcept {
        return value_;
    }

private:
    sqlite3* value_{};
};

/** Converts every uint64 bit pattern into SQLite's signed integer domain. */
[[nodiscard]] inline sqlite3_int64 sql_integer(std::uint64_t value) noexcept {
    return std::bit_cast<std::int64_t>(value);
}

/** Restores a uint64 bit pattern read from a SQLite signed integer. */
[[nodiscard]] inline std::uint64_t unsigned_integer(sqlite3_int64 value) noexcept {
    return std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

/** Binds one bool as canonical integer zero or one. */
[[nodiscard]] inline bool bind_bool(sqlite3_stmt* statement, int index, bool value) noexcept {
    return sqlite3_bind_int(statement, index, value ? 1 : 0) == SQLITE_OK;
}

/** Binds an unsigned 64-bit bit pattern without losing its high bit. */
[[nodiscard]] inline bool
bind_u64(sqlite3_stmt* statement, int index, std::uint64_t value) noexcept {
    return sqlite3_bind_int64(statement, index, sql_integer(value)) == SQLITE_OK;
}

/** Binds a nonnegative count after proving it fits SQLite's signed range. */
[[nodiscard]] inline bool
bind_count(sqlite3_stmt* statement, int index, std::size_t value) noexcept {
    if (value > static_cast<std::size_t>((std::numeric_limits<sqlite3_int64>::max)())) {
        return false;
    }
    return sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

/** Reads a canonical SQLite boolean. */
[[nodiscard]] inline bool column_bool(sqlite3_stmt* statement, int index, bool& output) noexcept {
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value != 0 && value != 1) {
        return false;
    }
    output = value != 0;
    return true;
}

/** Reads a size bounded by one caller-provided capacity. */
[[nodiscard]] inline bool column_count(sqlite3_stmt* statement,
                                       int index,
                                       std::size_t capacity,
                                       std::size_t& output) noexcept {
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0 || static_cast<std::uint64_t>(value) > capacity) {
        return false;
    }
    output = static_cast<std::size_t>(value);
    return true;
}

/** Reads one uint8 scalar without truncation. */
[[nodiscard]] inline bool column_u8(sqlite3_stmt* statement,
                                    int index,
                                    std::uint8_t& output) noexcept {
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0 || value > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint8_t>(value);
    return true;
}

/** Reads one uint32 scalar without truncation. */
[[nodiscard]] inline bool column_u32(sqlite3_stmt* statement,
                                     int index,
                                     std::uint32_t& output) noexcept {
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0
        || static_cast<std::uint64_t>(value)
               > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/** Reads one int32 scalar without truncation. */
[[nodiscard]] inline bool column_i32(sqlite3_stmt* statement,
                                     int index,
                                     std::int32_t& output) noexcept {
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    output = static_cast<std::int32_t>(value);
    return true;
}

/** Reads one finite float without silently overflowing it. */
[[nodiscard]] inline bool column_float(sqlite3_stmt* statement,
                                       int index,
                                       float& output) noexcept {
    const double value = sqlite3_column_double(statement, index);
    if (!std::isfinite(value)
        || value < static_cast<double>((std::numeric_limits<float>::lowest)())
        || value > static_cast<double>((std::numeric_limits<float>::max)())) {
        return false;
    }
    output = static_cast<float>(value);
    return true;
}

/** Creates the first normalized account schema. */
[[nodiscard]] inline bool create_schema(Database& database) noexcept {
    static constexpr const char* kSchema = R"sql(
CREATE TABLE account_state (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    format_version INTEGER NOT NULL,
    primary_soid INTEGER NOT NULL,
    dismantle_reward_count INTEGER NOT NULL CHECK (dismantle_reward_count >= 0),
    profile_item_count INTEGER NOT NULL CHECK (profile_item_count >= 0),
    character_count INTEGER NOT NULL CHECK (character_count >= 0),
    settings_payload BLOB NOT NULL,
    updated_unix_seconds INTEGER NOT NULL
);
CREATE TABLE dismantle_rewards (
    account_id INTEGER NOT NULL,
    position INTEGER NOT NULL CHECK (position >= 0),
    definition_hash INTEGER NOT NULL CHECK (definition_hash >= 0),
    quantity INTEGER NOT NULL,
    tier_mask INTEGER NOT NULL CHECK (tier_mask BETWEEN 0 AND 255),
    class_mask INTEGER NOT NULL CHECK (class_mask BETWEEN 0 AND 255),
    masterwork INTEGER NOT NULL CHECK (masterwork BETWEEN 0 AND 255),
    PRIMARY KEY (account_id, position),
    FOREIGN KEY (account_id) REFERENCES account_state(singleton) ON DELETE CASCADE
);
CREATE TABLE profile_items (
    account_id INTEGER NOT NULL,
    position INTEGER NOT NULL CHECK (position >= 0),
    instance_soid INTEGER NOT NULL,
    definition_hash INTEGER NOT NULL CHECK (definition_hash >= 0),
    quantity INTEGER NOT NULL,
    mutation_serial INTEGER NOT NULL,
    PRIMARY KEY (account_id, position),
    FOREIGN KEY (account_id) REFERENCES account_state(singleton) ON DELETE CASCADE
);
CREATE TABLE characters (
    account_id INTEGER NOT NULL,
    position INTEGER NOT NULL CHECK (position >= 0),
    soid INTEGER NOT NULL,
    selected INTEGER NOT NULL CHECK (selected IN (0, 1)),
    race INTEGER NOT NULL CHECK (race BETWEEN 0 AND 255),
    gender INTEGER NOT NULL CHECK (gender BETWEEN 0 AND 255),
    character_class INTEGER NOT NULL CHECK (character_class BETWEEN 0 AND 255),
    level INTEGER NOT NULL CHECK (level BETWEEN 0 AND 255),
    accepted INTEGER NOT NULL CHECK (accepted IN (0, 1)),
    preview_available INTEGER NOT NULL CHECK (preview_available IN (0, 1)),
    appearance_value REAL NOT NULL,
    last_orbited_destination INTEGER NOT NULL CHECK (last_orbited_destination >= 0),
    content_bypass INTEGER NOT NULL CHECK (content_bypass IN (0, 1)),
    acquired_subclass_ability_mask INTEGER NOT NULL,
    inventory_count INTEGER NOT NULL CHECK (inventory_count >= 0),
    next_inventory_serial INTEGER NOT NULL CHECK (next_inventory_serial >= 0),
    PRIMARY KEY (account_id, position),
    FOREIGN KEY (account_id) REFERENCES account_state(singleton) ON DELETE CASCADE
);
CREATE TABLE character_items (
    account_id INTEGER NOT NULL,
    character_position INTEGER NOT NULL,
    location INTEGER NOT NULL CHECK (location IN (0, 1)),
    position INTEGER NOT NULL CHECK (position >= 0),
    instance_soid INTEGER NOT NULL,
    definition_hash INTEGER NOT NULL CHECK (definition_hash >= 0),
    item_level INTEGER NOT NULL,
    quantity INTEGER NOT NULL,
    mutation_serial INTEGER NOT NULL,
    flags INTEGER NOT NULL CHECK (flags >= 0),
    socket_policy INTEGER NOT NULL CHECK (socket_policy BETWEEN 0 AND 255),
    plug_count INTEGER NOT NULL CHECK (plug_count >= 0),
    movement_ability_entry INTEGER NOT NULL CHECK (movement_ability_entry BETWEEN 0 AND 255),
    grenade_ability_entry INTEGER NOT NULL CHECK (grenade_ability_entry BETWEEN 0 AND 255),
    super_ability_entry INTEGER NOT NULL CHECK (super_ability_entry BETWEEN 0 AND 255),
    melee_ability_entry INTEGER NOT NULL CHECK (melee_ability_entry BETWEEN 0 AND 255),
    class_ability_entry INTEGER NOT NULL CHECK (class_ability_entry BETWEEN 0 AND 255),
    PRIMARY KEY (account_id, character_position, location, position),
    FOREIGN KEY (account_id, character_position)
        REFERENCES characters(account_id, position) ON DELETE CASCADE
);
CREATE TABLE item_plugs (
    account_id INTEGER NOT NULL,
    character_position INTEGER NOT NULL,
    location INTEGER NOT NULL,
    item_position INTEGER NOT NULL,
    plug_position INTEGER NOT NULL CHECK (plug_position >= 0),
    definition_hash INTEGER CHECK (definition_hash >= 0),
    PRIMARY KEY (
        account_id,
        character_position,
        location,
        item_position,
        plug_position
    ),
    FOREIGN KEY (account_id, character_position, location, item_position)
        REFERENCES character_items(account_id, character_position, location, position)
        ON DELETE CASCADE
);
)sql";
    if (!database.execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    const bool created = database.execute(kSchema)
                         && database.execute("PRAGMA user_version = 1;")
                         && database.execute("COMMIT;");
    if (!created) {
        (void)database.execute("ROLLBACK;");
    }
    return created;
}

/** Opens a database, creates schema v1 when empty, and rejects every unknown version. */
[[nodiscard]] inline OpenResult open_database(void* module, Database& database) noexcept {
    if (!database.open(module)) {
        return OpenResult::failed;
    }
    Statement version(database.get(), "PRAGMA user_version;");
    if (version.get() == nullptr || sqlite3_step(version.get()) != SQLITE_ROW) {
        return OpenResult::failed;
    }
    const int current = sqlite3_column_int(version.get(), 0);
    if (sqlite3_step(version.get()) != SQLITE_DONE) {
        return OpenResult::failed;
    }
    if (current == 0) {
        return database.configure_supported_schema() && create_schema(database)
                   ? OpenResult::opened
                   : OpenResult::failed;
    }
    if (current > kSchemaVersion) {
        return OpenResult::newer;
    }
    if (current != kSchemaVersion) {
        return OpenResult::failed;
    }
    return OpenResult::opened;
}

} // namespace sunrise::state::persistence::sqlite::detail
