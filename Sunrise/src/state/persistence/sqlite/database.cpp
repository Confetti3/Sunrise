#include "database.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#if defined(_MSC_VER)
#pragma comment(lib, "winsqlite3.lib")
#endif

namespace sunrise::state::persistence::sqlite {
namespace {

[[nodiscard]] bool to_utf8(std::wstring_view value, std::array<char, 32768>& output) noexcept {
    if (value.empty() || value.size() >= output.size()
        || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int sourceLength = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           value.data(),
                                           sourceLength,
                                           output.data(),
                                           static_cast<int>(output.size() - 1),
                                           nullptr,
                                           nullptr);
    if (length <= 0) {
        return false;
    }
    output[static_cast<std::size_t>(length)] = '\0';
    return true;
}

[[nodiscard]] bool read_user_version(sqlite3* database, int& output) noexcept {
    Statement statement(database, "PRAGMA user_version;");
    if (statement.get() == nullptr || sqlite3_step(statement.get()) != SQLITE_ROW
        || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement.get(), 0);
    if (value < 0 || value > (std::numeric_limits<int>::max)()
        || sqlite3_step(statement.get()) != SQLITE_DONE) {
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

[[nodiscard]] bool read_application_id(sqlite3* database, int& output) noexcept {
    Statement statement(database, "PRAGMA application_id;");
    if (statement.get() == nullptr || sqlite3_step(statement.get()) != SQLITE_ROW
        || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement.get(), 0);
    if (value < (std::numeric_limits<int>::min)()
        || value > (std::numeric_limits<int>::max)()
        || sqlite3_step(statement.get()) != SQLITE_DONE) {
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

[[nodiscard]] bool compatible_identity(int version, int applicationId) noexcept {
    if (applicationId != 0 && applicationId != kApplicationId) {
        return false;
    }
    return version < kSchemaVersion || applicationId == kApplicationId;
}

[[nodiscard]] bool known_table_set(sqlite3* database, int version) noexcept {
    constexpr std::array<const char*, 6> v1{
        "account_state", "character_items", "characters", "dismantle_rewards",
        "item_plugs", "profile_items"};
    constexpr std::array<const char*, 15> v2{
        "account_state", "activity_arrival_overrides", "activity_defaults", "character_items",
        "characters", "dismantle_rewards", "entitlements", "family5_flag_overrides",
        "family5_value_overrides", "item_plugs", "profile_items", "store_metadata",
        "unlock_flag_runs", "unlock_objective_values", "unlock_progressions"};
    Statement tables(database,
                     "SELECT name FROM sqlite_master WHERE type='table' "
                     "AND name NOT LIKE 'sqlite_%' ORDER BY name;");
    if (tables.get() == nullptr) {
        return false;
    }
    const auto check = [&tables](const auto& expected) noexcept {
        for (const char* name : expected) {
            if (sqlite3_step(tables.get()) != SQLITE_ROW
                || sqlite3_column_type(tables.get(), 0) != SQLITE_TEXT
                || std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(tables.get(), 0)),
                               name) != 0) {
                return false;
            }
        }
        return sqlite3_step(tables.get()) == SQLITE_DONE;
    };
    if (version == 0) {
        return sqlite3_step(tables.get()) == SQLITE_DONE;
    }
    if (version == 1) {
        return check(v1);
    }
    return version == kSchemaVersion && check(v2);
}

[[nodiscard]] bool open_connection(const char* path,
                                   int flags,
                                   sqlite3*& output) noexcept {
    output = nullptr;
    if (sqlite3_open_v2(path, &output, flags, nullptr) != SQLITE_OK || output == nullptr) {
        if (output != nullptr) {
            (void)sqlite3_close(output);
            output = nullptr;
        }
        return false;
    }
    (void)sqlite3_extended_result_codes(output, 1);
    if (sqlite3_busy_timeout(output, kBusyTimeoutMilliseconds) != SQLITE_OK) {
        (void)sqlite3_close(output);
        output = nullptr;
        return false;
    }
    return true;
}

} // namespace

Statement::Statement(sqlite3* database, const char* sql) noexcept {
    if (database != nullptr && sql != nullptr) {
        (void)sqlite3_prepare_v2(database, sql, -1, &value_, nullptr);
    }
}

Statement::~Statement() noexcept {
    if (value_ != nullptr) {
        (void)sqlite3_finalize(value_);
    }
}

sqlite3_stmt* Statement::get() const noexcept {
    return value_;
}

bool Statement::reset() noexcept {
    return value_ != nullptr && sqlite3_reset(value_) == SQLITE_OK
           && sqlite3_clear_bindings(value_) == SQLITE_OK;
}

Database::~Database() noexcept {
    if (!close() && !healthy_) {
        (void)close_unhealthy();
    }
}

OpenResult Database::open(std::wstring_view path) noexcept {
    if (!close()) {
        return OpenResult::failed;
    }
    std::array<wchar_t, 32768> widePath{};
    if (path.empty() || path.size() >= widePath.size()) {
        return OpenResult::failed;
    }
    for (std::size_t index = 0; index < path.size(); ++index) {
        widePath[index] = path[index];
    }
    std::array<char, 32768> utf8Path{};
    if (!to_utf8(path, utf8Path)) {
        return OpenResult::failed;
    }

    const DWORD attributes = GetFileAttributesW(widePath.data());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (exists && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return OpenResult::failed;
    }

    // Existing files are probed read-only first. In particular, a newer user_version never
    // reaches writable pragmas and never causes SQLite to create WAL/SHM sidecars.
    if (exists) {
        sqlite3* probe = nullptr;
        if (!open_connection(utf8Path.data(), SQLITE_OPEN_READONLY, probe)) {
            return OpenResult::failed;
        }
        int version = -1;
        int applicationId = 0;
        if (!read_user_version(probe, version)
            || !read_application_id(probe, applicationId)) {
            (void)sqlite3_close(probe);
            return OpenResult::failed;
        }
        const bool identityCompatible = compatible_identity(version, applicationId);
        if (!identityCompatible || version > kSchemaVersion
            || !known_table_set(probe, version)) {
            value_ = probe;
            schemaVersion_ = version;
            writable_ = false;
            return identityCompatible && version > kSchemaVersion ? OpenResult::newer
                                                                  : OpenResult::failed;
        }
        (void)sqlite3_close(probe);
    }

    sqlite3* opened = nullptr;
    if (!open_connection(utf8Path.data(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, opened)) {
        return OpenResult::failed;
    }
    int version = -1;
    int applicationId = 0;
    if (!read_user_version(opened, version)
        || !read_application_id(opened, applicationId)) {
        (void)sqlite3_close(opened);
        return OpenResult::failed;
    }
    const bool identityCompatible = compatible_identity(version, applicationId);
    if (!identityCompatible || version > kSchemaVersion
        || !known_table_set(opened, version)) {
        (void)sqlite3_close(opened);
        // Retain only a read-only handle for incompatible or future databases.
        if (!open_connection(utf8Path.data(), SQLITE_OPEN_READONLY, opened)
            || !read_user_version(opened, version)
            || !read_application_id(opened, applicationId)) {
            if (opened != nullptr) {
                (void)sqlite3_close(opened);
            }
            return OpenResult::failed;
        }
        value_ = opened;
        schemaVersion_ = version;
        writable_ = false;
        return identityCompatible && version > kSchemaVersion ? OpenResult::newer
                                                              : OpenResult::failed;
    }
    value_ = opened;
    schemaVersion_ = version;
    writable_ = true;
    return version >= 0 && version <= kSchemaVersion ? OpenResult::opened : OpenResult::failed;
}

bool Database::close() noexcept {
    if (value_ == nullptr) {
        schemaVersion_ = -1;
        writable_ = false;
        transactionActive_ = false;
        healthy_ = true;
        return true;
    }
    if (transactionActive_) {
        return false;
    }
    if (sqlite3_close(value_) != SQLITE_OK) {
        // Keep every field truthful: callers can finalize the outstanding statement and retry.
        return false;
    }
    value_ = nullptr;
    schemaVersion_ = -1;
    writable_ = false;
    transactionActive_ = false;
    healthy_ = true;
    return true;
}

bool Database::close_unhealthy() noexcept {
    if (value_ == nullptr) {
        return true;
    }
    if (healthy_) {
        return false;
    }
    // close_v2 is the explicit fail-closed path after rollback/connection recovery failed. It
    // marks the handle unusable and defers final destruction until any remaining statements end.
    if (sqlite3_close_v2(value_) != SQLITE_OK) {
        return false;
    }
    value_ = nullptr;
    schemaVersion_ = -1;
    writable_ = false;
    transactionActive_ = false;
    return true;
}

OpenResult Database::inspect_schema() noexcept {
    if (value_ == nullptr) {
        return OpenResult::failed;
    }
    int version = -1;
    int applicationId = 0;
    if (!read_user_version(value_, version)
        || !read_application_id(value_, applicationId)) {
        return OpenResult::failed;
    }
    schemaVersion_ = version;
    if (!compatible_identity(version, applicationId)) {
        return OpenResult::failed;
    }
    if (version > kSchemaVersion) {
        return OpenResult::newer;
    }
    if (!known_table_set(value_, version)) {
        return OpenResult::failed;
    }
    return version >= 0 && version <= kSchemaVersion ? OpenResult::opened : OpenResult::failed;
}

bool Database::configure_supported_schema() noexcept {
    return value_ != nullptr && writable_ && schemaVersion_ >= 0
           && schemaVersion_ <= kSchemaVersion && execute("PRAGMA foreign_keys = ON;")
           && execute("PRAGMA synchronous = FULL;");
}

bool Database::enable_wal() noexcept {
    if (value_ == nullptr || !writable_ || schemaVersion_ < 0 || schemaVersion_ > kSchemaVersion) {
        return false;
    }
    // A filesystem may reject WAL; the rollback journal remains a safe supported fallback.
    (void)execute("PRAGMA journal_mode = WAL;");
    return true;
}

OpenResult Database::initialize_schema() noexcept {
    const OpenResult inspected = inspect_schema();
    if (inspected != OpenResult::opened || !writable_) {
        return inspected == OpenResult::newer ? inspected : OpenResult::failed;
    }
    if (!configure_supported_schema()) {
        return OpenResult::failed;
    }
    if (schemaVersion_ < kSchemaVersion && !initialize_state_schema(*this)) {
        return OpenResult::failed;
    }
    return inspect_schema();
}

bool Database::is_open() const noexcept {
    return value_ != nullptr;
}

bool Database::is_writable() const noexcept {
    return writable_;
}

bool Database::healthy() const noexcept {
    return healthy_;
}

bool Database::transaction_active() const noexcept {
    return transactionActive_;
}

int Database::schema_version() const noexcept {
    return schemaVersion_;
}

sqlite3* Database::get() const noexcept {
    return value_;
}

bool Database::execute(const char* sql) noexcept {
    return value_ != nullptr && sql != nullptr
           && sqlite3_exec(value_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

Transaction::Transaction(Database& database) noexcept : database_(&database) {
    active_ = database_->healthy() && database_->is_writable()
              && database_->execute("BEGIN IMMEDIATE;");
    if (active_) {
        database_->mark_transaction_started();
    }
}

Transaction::~Transaction() noexcept {
    if (!rollback() && database_ != nullptr) {
        database_->mark_unhealthy();
    }
}

bool Transaction::active() const noexcept {
    return active_;
}

bool Transaction::commit() noexcept {
    if (!active_ || database_ == nullptr || !database_->execute("COMMIT;")) {
        return false;
    }
    active_ = false;
    database_->mark_transaction_finished();
    return true;
}

bool Transaction::rollback() noexcept {
    if (!active_) {
        return true;
    }
    if (database_ == nullptr || !database_->execute("ROLLBACK;")) {
        // Leave active_ and Database::transactionActive_ untouched so a caller/destructor can retry.
        return false;
    }
    active_ = false;
    database_->mark_transaction_finished();
    return true;
}

void Database::mark_transaction_started() noexcept {
    transactionActive_ = true;
}

void Database::mark_transaction_finished() noexcept {
    transactionActive_ = false;
}

void Database::mark_unhealthy() noexcept {
    healthy_ = false;
}
sqlite3_int64 sql_integer(std::uint64_t value) noexcept {
    return std::bit_cast<std::int64_t>(value);
}

std::uint64_t unsigned_integer(sqlite3_int64 value) noexcept {
    return std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

bool bind_bool(sqlite3_stmt* statement, int index, bool value) noexcept {
    return statement != nullptr && sqlite3_bind_int(statement, index, value ? 1 : 0) == SQLITE_OK;
}

bool bind_u64(sqlite3_stmt* statement, int index, std::uint64_t value) noexcept {
    return statement != nullptr
           && sqlite3_bind_int64(statement, index, sql_integer(value)) == SQLITE_OK;
}

bool bind_u32(sqlite3_stmt* statement, int index, std::uint32_t value) noexcept {
    return statement != nullptr
           && sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

bool bind_i32(sqlite3_stmt* statement, int index, std::int32_t value) noexcept {
    return statement != nullptr
           && sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

bool bind_count(sqlite3_stmt* statement, int index, std::size_t value) noexcept {
    if (statement == nullptr
        || value > static_cast<std::size_t>((std::numeric_limits<sqlite3_int64>::max)())) {
        return false;
    }
    return sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

bool column_bool(sqlite3_stmt* statement, int index, bool& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value != 0 && value != 1) {
        return false;
    }
    output = value != 0;
    return true;
}

bool column_u64(sqlite3_stmt* statement, int index, std::uint64_t& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    output = unsigned_integer(sqlite3_column_int64(statement, index));
    return true;
}

bool column_count(sqlite3_stmt* statement,
                  int index,
                  std::size_t capacity,
                  std::size_t& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0 || static_cast<std::uint64_t>(value) > capacity) {
        return false;
    }
    output = static_cast<std::size_t>(value);
    return true;
}

bool column_u8(sqlite3_stmt* statement, int index, std::uint8_t& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0 || value > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint8_t>(value);
    return true;
}

bool column_u32(sqlite3_stmt* statement, int index, std::uint32_t& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < 0
        || static_cast<std::uint64_t>(value) > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

bool column_i32(sqlite3_stmt* statement, int index, std::int32_t& output) noexcept {
    if (statement == nullptr || sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 value = sqlite3_column_int64(statement, index);
    if (value < (std::numeric_limits<std::int32_t>::min)()
        || value > (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    output = static_cast<std::int32_t>(value);
    return true;
}

bool column_float(sqlite3_stmt* statement, int index, float& output) noexcept {
    if (statement == nullptr) {
        return false;
    }
    const int type = sqlite3_column_type(statement, index);
    if (type != SQLITE_FLOAT && type != SQLITE_INTEGER) {
        return false;
    }
    const double value = sqlite3_column_double(statement, index);
    if (!std::isfinite(value)
        || value < static_cast<double>((std::numeric_limits<float>::lowest)())
        || value > static_cast<double>((std::numeric_limits<float>::max)())) {
        return false;
    }
    output = static_cast<float>(value);
    return std::isfinite(output);
}

} // namespace sunrise::state::persistence::sqlite
