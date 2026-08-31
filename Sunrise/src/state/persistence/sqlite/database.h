#pragma once

#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "schema.h"

namespace sunrise::state::persistence::sqlite {

/** Version of the normalized account row contract. */
inline constexpr int kAccountFormatVersion = 1;
/** Version of the explicitly encoded settings payload. */
inline constexpr std::uint32_t kSettingsPayloadVersion = 1;
/** Upper bound for one encoded settings payload. */
inline constexpr std::size_t kSettingsPayloadCapacity = 1024;
/** SQLite busy timeout used by every store connection. */
inline constexpr int kBusyTimeoutMilliseconds = 3'000;
/** Equipment and inventory discriminators in character_items.location. */
inline constexpr int kEquipmentItemLocation = 0;
inline constexpr int kInventoryItemLocation = 1;

/** Result of opening or inspecting a database schema. */
enum class OpenResult {
    opened,
    newer,
    failed,
};

/** Owns one prepared statement and finalizes it on every exit path. */
class Statement final {
public:
    Statement(sqlite3* database, const char* sql) noexcept;
    ~Statement() noexcept;

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept;
    [[nodiscard]] bool reset() noexcept;

private:
    sqlite3_stmt* value_{};
};

/** Owns one WinSQLite connection. */
class Database final {
public:
    Database() noexcept = default;
    ~Database() noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /** Opens a caller-provided UTF-16 path after a read-only version probe. */
    [[nodiscard]] OpenResult open(std::wstring_view path) noexcept;
    /** Closes the connection; on SQLITE_BUSY/error the handle and state are retained. */
    [[nodiscard]] bool close() noexcept;
    /** Explicit fail-closed disposal for a connection marked unhealthy. */
    [[nodiscard]] bool close_unhealthy() noexcept;
    /** Reads user_version without changing connection or journal state. */
    [[nodiscard]] OpenResult inspect_schema() noexcept;
    /** Enables supported writable pragmas without changing journal mode. */
    [[nodiscard]] bool configure_supported_schema() noexcept;
    /** Switches a supported writable connection to WAL as a best effort. */
    [[nodiscard]] bool enable_wal() noexcept;
    /** Creates the latest supported schema in a rollback-safe transaction from user_version zero. */
    [[nodiscard]] OpenResult initialize_schema() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_writable() const noexcept;
    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] bool transaction_active() const noexcept;
    [[nodiscard]] int schema_version() const noexcept;
    [[nodiscard]] sqlite3* get() const noexcept;
    [[nodiscard]] bool execute(const char* sql) noexcept;

private:
    friend class Transaction;

    void mark_transaction_started() noexcept;
    void mark_transaction_finished() noexcept;
    void mark_unhealthy() noexcept;

    sqlite3* value_{};
    int schemaVersion_{-1};
    bool writable_{};
    bool healthy_{true};
    bool transactionActive_{};
};

/** Begins an immediate transaction and rolls it back unless committed. */
class Transaction final {
public:
    explicit Transaction(Database& database) noexcept;
    ~Transaction() noexcept;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool commit() noexcept;
    /** Returns true only when rollback completed; otherwise remains active for retry. */
    [[nodiscard]] bool rollback() noexcept;

private:
    Database* database_{};
    bool active_{};
};

namespace detail {
using ::sunrise::state::persistence::sqlite::Database;
using ::sunrise::state::persistence::sqlite::OpenResult;
using ::sunrise::state::persistence::sqlite::Statement;
using ::sunrise::state::persistence::sqlite::Transaction;
inline constexpr int kSchemaVersion = ::sunrise::state::persistence::sqlite::kSchemaVersion;
inline constexpr int kAccountFormatVersion =
    ::sunrise::state::persistence::sqlite::kAccountFormatVersion;
inline constexpr std::uint32_t kSettingsPayloadVersion =
    ::sunrise::state::persistence::sqlite::kSettingsPayloadVersion;
inline constexpr std::size_t kSettingsPayloadCapacity =
    ::sunrise::state::persistence::sqlite::kSettingsPayloadCapacity;
inline constexpr int kBusyTimeoutMilliseconds =
    ::sunrise::state::persistence::sqlite::kBusyTimeoutMilliseconds;
inline constexpr int kEquipmentItemLocation =
    ::sunrise::state::persistence::sqlite::kEquipmentItemLocation;
inline constexpr int kInventoryItemLocation =
    ::sunrise::state::persistence::sqlite::kInventoryItemLocation;
} // namespace detail

/** Converts every uint64 bit pattern into SQLite's signed integer domain. */
[[nodiscard]] sqlite3_int64 sql_integer(std::uint64_t value) noexcept;
/** Restores a uint64 bit pattern read from SQLite's signed integer domain. */
[[nodiscard]] std::uint64_t unsigned_integer(sqlite3_int64 value) noexcept;

[[nodiscard]] bool bind_bool(sqlite3_stmt* statement, int index, bool value) noexcept;
[[nodiscard]] bool bind_u64(sqlite3_stmt* statement, int index, std::uint64_t value) noexcept;
[[nodiscard]] bool bind_u32(sqlite3_stmt* statement, int index, std::uint32_t value) noexcept;
[[nodiscard]] bool bind_i32(sqlite3_stmt* statement, int index, std::int32_t value) noexcept;
[[nodiscard]] bool bind_count(sqlite3_stmt* statement, int index, std::size_t value) noexcept;

[[nodiscard]] bool column_bool(sqlite3_stmt* statement, int index, bool& output) noexcept;
[[nodiscard]] bool column_u64(sqlite3_stmt* statement, int index, std::uint64_t& output) noexcept;
[[nodiscard]] bool column_count(sqlite3_stmt* statement,
                                int index,
                                std::size_t capacity,
                                std::size_t& output) noexcept;
[[nodiscard]] bool column_u8(sqlite3_stmt* statement,
                             int index,
                             std::uint8_t& output) noexcept;
[[nodiscard]] bool column_u32(sqlite3_stmt* statement,
                              int index,
                              std::uint32_t& output) noexcept;
[[nodiscard]] bool column_i32(sqlite3_stmt* statement,
                              int index,
                              std::int32_t& output) noexcept;
[[nodiscard]] bool column_float(sqlite3_stmt* statement,
                                int index,
                                float& output) noexcept;

namespace detail {
using ::sunrise::state::persistence::sqlite::bind_bool;
using ::sunrise::state::persistence::sqlite::bind_count;
using ::sunrise::state::persistence::sqlite::bind_i32;
using ::sunrise::state::persistence::sqlite::bind_u32;
using ::sunrise::state::persistence::sqlite::bind_u64;
using ::sunrise::state::persistence::sqlite::column_bool;
using ::sunrise::state::persistence::sqlite::column_count;
using ::sunrise::state::persistence::sqlite::column_float;
using ::sunrise::state::persistence::sqlite::column_i32;
using ::sunrise::state::persistence::sqlite::column_u32;
using ::sunrise::state::persistence::sqlite::column_u64;
using ::sunrise::state::persistence::sqlite::column_u8;
using ::sunrise::state::persistence::sqlite::sql_integer;
using ::sunrise::state::persistence::sqlite::unsigned_integer;
} // namespace detail

} // namespace sunrise::state::persistence::sqlite
