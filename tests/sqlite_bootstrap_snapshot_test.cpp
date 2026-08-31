#include <Windows.h>
#include <winsqlite/winsqlite3.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/settings/settings.h"
#include "state/persistence/sqlite/bootstrap_snapshot.h"
#include "state/persistence/sqlite/account_snapshot.h"

namespace fs = std::filesystem;
namespace sqlite = sunrise::state::persistence::sqlite;

namespace sunrise::core::log {
Settings defaults() noexcept { return {}; }
void early(std::string_view) noexcept {}
}

namespace {

[[nodiscard]] bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "sqlite bootstrap snapshot test failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string normalize_sql(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (char byte : input) if (byte != '\r') output.push_back(byte);
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) output.pop_back();
    return output;
}

[[nodiscard]] std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> chars{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    std::vector<std::byte> result(chars.size());
    for (std::size_t index = 0; index < chars.size(); ++index) {
        result[index] = static_cast<std::byte>(chars[index]);
    }
    return result;
}

[[nodiscard]] bool execute(sqlite3* database, std::string_view sql) {
    return sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

[[nodiscard]] bool query_integer(sqlite3* database, const char* sql, sqlite3_int64& output) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK
        || statement == nullptr || sqlite3_step(statement) != SQLITE_ROW
        || sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
        if (statement != nullptr) sqlite3_finalize(statement);
        return false;
    }
    output = sqlite3_column_int64(statement, 0);
    const bool complete = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return complete;
}

[[nodiscard]] bool equivalent(const sunrise::core::settings::Settings& json,
                              const sqlite::BootstrapSnapshot& sql) {
    const auto& leftEntitlements = json.server.entitlements;
    if (leftEntitlements.count != sql.entitlements.count) return false;
    for (std::size_t index = 0; index < leftEntitlements.count; ++index) {
        const auto& left = leftEntitlements.entries[index];
        const auto& right = sql.entitlements.entries[index];
        if (left.nameLength != right.nameLength || left.ownership != right.ownership
            || sunrise::state::entitlements::name_of(left)
                   != sunrise::state::entitlements::name_of(right)) return false;
    }
    const auto& leftUnlocks = json.initialUnlocks;
    if (leftUnlocks.accountFlags != sql.unlocks.accountFlags
        || leftUnlocks.profileFlags != sql.unlocks.profileFlags
        || leftUnlocks.characterFlags != sql.unlocks.characterFlags
        || leftUnlocks.objectiveValues != sql.unlocks.objectiveValues
        || leftUnlocks.characterObjectFlags != sql.unlocks.characterObjectFlags
        || leftUnlocks.characterObjectValues != sql.unlocks.characterObjectValues
        || leftUnlocks.accountProgressions != sql.unlocks.accountProgressions
        || leftUnlocks.characterProgressions != sql.unlocks.characterProgressions) return false;
    const auto& leftFamily = json.initialFamily5;
    if (leftFamily.flagCount != sql.family5.flagCount
        || leftFamily.valueCount != sql.family5.valueCount) return false;
    for (std::size_t index = 0; index < leftFamily.flagCount; ++index) {
        if (leftFamily.flags[index].slot != sql.family5.flags[index].slot
            || leftFamily.flags[index].value != sql.family5.flags[index].value) return false;
    }
    for (std::size_t index = 0; index < leftFamily.valueCount; ++index) {
        if (leftFamily.values[index].slot != sql.family5.values[index].slot
            || leftFamily.values[index].value != sql.family5.values[index].value) return false;
    }
    const auto& leftActivity = json.initialActivityDefaults;
    const auto& rightActivity = sql.activityDefaults;
    const auto& leftSelection = leftActivity.defaultDestination.selection;
    const auto& rightSelection = rightActivity.defaultDestination.selection;
    const auto& leftFallback = leftActivity.defaultDestination.fallback;
    const auto& rightFallback = rightActivity.defaultDestination.fallback;
    if (leftSelection.packageNameLength != rightSelection.packageNameLength
        || leftSelection.reason != rightSelection.reason
        || leftSelection.previousActivityIndex != rightSelection.previousActivityIndex
        || leftSelection.activityIndex != rightSelection.activityIndex
        || !std::equal(leftSelection.packageName.begin(),
                       leftSelection.packageName.begin() + leftSelection.packageNameLength,
                       rightSelection.packageName.begin())
        || leftFallback.bubbleCount != rightFallback.bubbleCount
        || leftFallback.statefulBubbleMask != rightFallback.statefulBubbleMask
        || leftFallback.initialSliceSet != rightFallback.initialSliceSet
        || leftFallback.spawnSetHash != rightFallback.spawnSetHash
        || leftActivity.rosterKeyFromIdentity != rightActivity.rosterKeyFromIdentity
        || leftActivity.rosterKeyOnAllSlots != rightActivity.rosterKeyOnAllSlots
        || leftActivity.arrivalOverrideCount != rightActivity.arrivalOverrideCount) return false;
    for (std::size_t index = 0; index < leftActivity.arrivalOverrideCount; ++index) {
        const auto& left = leftActivity.arrivalOverrides[index];
        const auto& right = rightActivity.arrivalOverrides[index];
        if (left.nameLength != right.nameLength || left.hasBubble != right.hasBubble
            || left.bubble != right.bubble || left.hasSpawnSetHash != right.hasSpawnSetHash
            || left.spawnSetHash != right.spawnSetHash
            || std::string_view(left.name.data(), left.nameLength)
                   != std::string_view(right.name.data(), right.nameLength)) return false;
    }
    return true;
}

[[nodiscard]] std::uint64_t canonical_hash(const sqlite::BootstrapSnapshot& snapshot) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto byte = [&hash](std::uint8_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    const auto integer = [&byte](std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            byte(static_cast<std::uint8_t>(value >> shift));
        }
    };
    integer(snapshot.entitlements.count);
    for (std::size_t index = 0; index < snapshot.entitlements.count; ++index) {
        const auto& row = snapshot.entitlements.entries[index];
        integer(row.nameLength);
        for (char value : sunrise::state::entitlements::name_of(row)) byte(value);
        integer(static_cast<std::uint8_t>(row.ownership));
    }
    for (int scope = 0; scope < 4; ++scope) {
        std::span<const std::uint8_t> bank;
        if (scope == 0) bank = snapshot.unlocks.accountFlags;
        if (scope == 1) bank = snapshot.unlocks.profileFlags;
        if (scope == 2) bank = snapshot.unlocks.characterFlags;
        if (scope == 3) bank = snapshot.unlocks.characterObjectFlags;
        for (std::uint8_t value : bank) byte(value);
    }
    for (std::int32_t value : snapshot.unlocks.objectiveValues) integer(static_cast<std::uint32_t>(value));
    for (std::int32_t value : snapshot.unlocks.characterObjectValues) integer(static_cast<std::uint32_t>(value));
    for (const auto* bank : {&snapshot.unlocks.accountProgressions,
                             &snapshot.unlocks.characterProgressions}) {
        for (const auto& row : *bank) for (std::int32_t value : row) integer(static_cast<std::uint32_t>(value));
    }
    integer(snapshot.family5.flagCount);
    for (std::size_t index = 0; index < snapshot.family5.flagCount; ++index) {
        integer(snapshot.family5.flags[index].slot);
        integer(snapshot.family5.flags[index].value);
    }
    integer(snapshot.family5.valueCount);
    for (std::size_t index = 0; index < snapshot.family5.valueCount; ++index) {
        integer(snapshot.family5.values[index].slot);
        integer(static_cast<std::uint32_t>(snapshot.family5.values[index].value));
    }
    const auto& activity = snapshot.activityDefaults;
    const auto& selection = activity.defaultDestination.selection;
    const auto& fallback = activity.defaultDestination.fallback;
    integer(static_cast<std::uint8_t>(selection.reason));
    integer(static_cast<std::uint16_t>(selection.previousActivityIndex));
    integer(static_cast<std::uint16_t>(selection.activityIndex));
    integer(selection.packageNameLength);
    for (std::size_t index = 0; index < selection.packageNameLength; ++index) {
        byte(static_cast<std::uint8_t>(selection.packageName[index]));
    }
    integer(fallback.bubbleCount);
    integer(fallback.statefulBubbleMask);
    integer(fallback.initialSliceSet);
    integer(fallback.spawnSetHash);
    integer(activity.rosterKeyFromIdentity);
    integer(activity.rosterKeyOnAllSlots);
    integer(activity.arrivalOverrideCount);
    for (std::size_t index = 0; index < activity.arrivalOverrideCount; ++index) {
        const auto& row = activity.arrivalOverrides[index];
        integer(row.nameLength);
        for (std::size_t character = 0; character < row.nameLength; ++character) byte(row.name[character]);
        integer(row.hasBubble);
        integer(row.bubble);
        integer(row.hasSpawnSetHash);
        integer(row.spawnSetHash);
    }
    return hash;
}

struct DatabaseFixture {
    fs::path path;
    sqlite::Database database;
};

[[nodiscard]] bool seed(DatabaseFixture& fixture, const std::string& seedSql) {
    return fixture.database.open(fixture.path.native()) == sqlite::OpenResult::opened
           && fixture.database.initialize_schema() == sqlite::OpenResult::opened
           && fixture.database.execute(seedSql.c_str());
}

[[nodiscard]] bool malformed_case(const fs::path& root,
                                  const std::string& seedSql,
                                  std::string_view name,
                                  std::string_view mutation) {
    DatabaseFixture fixture{root / (std::string(name) + ".sqlite3")};
    if (!seed(fixture, seedSql)
        || !fixture.database.execute("PRAGMA ignore_check_constraints = ON;")
        || !fixture.database.execute(std::string(mutation).c_str())) return false;
    sqlite::BootstrapSnapshot output{};
    return !sqlite::load_bootstrap(fixture.database, output);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const fs::path repo = argv[1];
    const fs::path authoritativeJson = argv[2];
    const std::string schemaV1 = read_text(repo / "Sunrise/resources/state_schema_v1.sql");
    const std::string migrationV1ToV2 =
        read_text(repo / "Sunrise/resources/state_migration_v1_to_v2.sql");
    const std::string seedSql = read_text(repo / "Sunrise/resources/default_state_seed.sql");
    const std::string jsonText = read_text(authoritativeJson);
    if (!check(!schemaV1.empty() && !migrationV1ToV2.empty() && !seedSql.empty()
                   && !jsonText.empty(),
               "read resources")
        || !check(normalize_sql(schemaV1) == normalize_sql(sqlite::schema_v1_sql()),
                  "compiled v1 schema matches resource")
        || !check(normalize_sql(migrationV1ToV2)
                      == normalize_sql(sqlite::migration_v1_to_v2_sql()),
                  "compiled v1-to-v2 migration matches resource")) return 1;

    sunrise::core::settings::Settings json{};
    if (!check(sunrise::core::settings::parse(jsonText, json), "production JSON parse")) return 1;

    const fs::path root = fs::temp_directory_path() / "sunrise-sqlite-bootstrap-test";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (!check(!error, "create temporary directory")) return 1;

    DatabaseFixture fresh{root / "fresh.sqlite3"};
    if (!check(seed(fresh, seedSql), "fresh v2 schema and seed")) return 1;
    sqlite3_int64 version = 0;
    sqlite3_int64 application = 0;
    if (!check(query_integer(fresh.database.get(), "PRAGMA user_version;", version) && version == 2,
               "fresh user_version")
        || !check(query_integer(fresh.database.get(), "PRAGMA application_id;", application)
                      && application == sqlite::kApplicationId,
                  "fresh application_id")) return 1;
    sqlite::BootstrapSnapshot loaded{};
    sqlite::StoreMetadata metadata{};
    if (!check(sqlite::load_bootstrap(fresh.database, loaded), "load SQL seed")
        || !check(sqlite::load_store_metadata(fresh.database, metadata)
                      && metadata.seedRevision == 8 && metadata.contentBuild == 86657
                      && metadata.legacySourceVersion == 8,
                  "load and pin seed metadata")
        || !check(equivalent(json, loaded), "SQL seed equals version-8 JSON")
        || !check(loaded.entitlements.count == 19, "pinned entitlement count")
        || !check(loaded.family5.flagCount == 39 && loaded.family5.valueCount == 8,
                  "pinned family-5 counts")
        || !check(loaded.activityDefaults.arrivalOverrideCount == 8, "pinned arrival count")) return 1;
    if (!check(canonical_hash(loaded) == 0xD394A58B0C030A4CULL,
               "pinned canonical bootstrap hash")) return 1;

    sunrise::state::AccountState seededAccount{};
    if (!check(sqlite::load_account(fresh.database, seededAccount) == sqlite::LoadResult::loaded,
               "load seeded account")) return 1;
    DatabaseFixture expectedAccount{root / "expected-account.sqlite3"};
    if (!check(expectedAccount.database.open(expectedAccount.path.native())
                       == sqlite::OpenResult::opened
                   && expectedAccount.database.initialize_schema() == sqlite::OpenResult::opened
                   && sqlite::save_account(expectedAccount.database, json.initialAccount)
                   && expectedAccount.database.close(),
               "encode expected JSON account")) return 1;
    std::string attach = "ATTACH DATABASE '" + expectedAccount.path.string() + "' AS expected;";
    if (!check(fresh.database.execute(attach.c_str()), "attach expected account")) return 1;
    constexpr std::array<std::string_view, 6> accountQueries{
        "SELECT format_version,primary_soid,dismantle_reward_count,profile_item_count,"
        "character_count,settings_payload FROM main.account_state",
        "SELECT * FROM main.dismantle_rewards",
        "SELECT * FROM main.profile_items",
        "SELECT * FROM main.characters",
        "SELECT * FROM main.character_items",
        "SELECT * FROM main.item_plugs"};
    constexpr std::array<std::string_view, 6> expectedQueries{
        "SELECT format_version,primary_soid,dismantle_reward_count,profile_item_count,"
        "character_count,settings_payload FROM expected.account_state",
        "SELECT * FROM expected.dismantle_rewards",
        "SELECT * FROM expected.profile_items",
        "SELECT * FROM expected.characters",
        "SELECT * FROM expected.character_items",
        "SELECT * FROM expected.item_plugs"};
    for (std::size_t index = 0; index < accountQueries.size(); ++index) {
        const std::string difference = "SELECT count(*) FROM (" + std::string(accountQueries[index])
                                       + " EXCEPT " + std::string(expectedQueries[index])
                                       + " UNION ALL SELECT * FROM ("
                                       + std::string(expectedQueries[index]) + " EXCEPT "
                                       + std::string(accountQueries[index]) + "));";
        sqlite3_int64 count = -1;
        if (!check(query_integer(fresh.database.get(), difference.c_str(), count) && count == 0,
                   "seeded account equals version-8 JSON")) return 1;
    }
    if (!check(fresh.database.execute("DETACH DATABASE expected;")
                   && sqlite::save_account(fresh.database, json.initialAccount),
               "save account beside bootstrap")) return 1;
    sqlite::BootstrapSnapshot afterAccountSave{};
    if (!check(sqlite::load_bootstrap(fresh.database, afterAccountSave)
                   && canonical_hash(afterAccountSave) == canonical_hash(loaded),
               "account save leaves bootstrap unchanged")) return 1;

    DatabaseFixture canonical{root / "canonical.sqlite3"};
    if (!check(seed(canonical, seedSql) && sqlite::save_bootstrap(canonical.database, loaded),
               "canonical snapshot save")) return 1;
    sqlite3_int64 characterRuns = 0;
    if (!check(query_integer(canonical.database.get(),
                             "SELECT count(*) FROM unlock_flag_runs WHERE scope=2;",
                             characterRuns)
                   && characterRuns == 2,
               "character flags compact to maximal runs")) return 1;
    if (!check(canonical.database.execute(
                   "CREATE TRIGGER fail_entitlement BEFORE INSERT ON entitlements "
                   "BEGIN SELECT RAISE(ABORT,'injected'); END;")
                   && !sqlite::save_bootstrap(canonical.database, loaded),
               "bootstrap save rollback injection")) return 1;
    sqlite::BootstrapSnapshot afterRollback{};
    if (!check(sqlite::load_bootstrap(canonical.database, afterRollback)
                   && canonical_hash(afterRollback) == canonical_hash(loaded),
               "failed save retains prior bootstrap")) return 1;

    const auto makeV1 = [](const fs::path& source, const fs::path& destination) {
        std::error_code copyError;
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, copyError);
        sqlite3* rawDatabase = nullptr;
        if (copyError || sqlite3_open_v2(destination.string().c_str(),
                                         &rawDatabase,
                                         SQLITE_OPEN_READWRITE,
                                         nullptr) != SQLITE_OK) return false;
        const bool converted = execute(rawDatabase,
            "DROP TABLE activity_arrival_overrides; DROP TABLE activity_defaults; "
            "DROP TABLE family5_value_overrides; DROP TABLE family5_flag_overrides; "
            "DROP TABLE unlock_progressions; DROP TABLE unlock_objective_values; "
            "DROP TABLE unlock_flag_runs; DROP TABLE entitlements; DROP TABLE store_metadata; "
            "PRAGMA application_id=0; PRAGMA user_version=1;");
        return sqlite3_close(rawDatabase) == SQLITE_OK && converted;
    };

    DatabaseFixture migration{root / "migration.sqlite3"};
    const fs::path migrationBefore = root / "migration-before.sqlite3";
    if (!check(makeV1(expectedAccount.path, migration.path)
                   && fs::copy_file(migration.path,
                                    migrationBefore,
                                    fs::copy_options::overwrite_existing,
                                    error)
                   && migration.database.open(migration.path.native()) == sqlite::OpenResult::opened
                   && migration.database.initialize_schema() == sqlite::OpenResult::opened,
               "full v1 to v2 migration")) return 1;
    attach = "ATTACH DATABASE '" + migrationBefore.string() + "' AS before_migration;";
    if (!check(migration.database.execute(attach.c_str()), "attach v1 migration source")) return 1;
    for (std::string_view table : {"account_state", "dismantle_rewards", "profile_items",
                                   "characters", "character_items", "item_plugs"}) {
        const std::string difference =
            "SELECT count(*) FROM (SELECT * FROM main." + std::string(table)
            + " EXCEPT SELECT * FROM before_migration." + std::string(table)
            + " UNION ALL SELECT * FROM (SELECT * FROM before_migration." + std::string(table)
            + " EXCEPT SELECT * FROM main." + std::string(table) + "));";
        sqlite3_int64 count = -1;
        if (!check(query_integer(migration.database.get(), difference.c_str(), count) && count == 0,
                   "migration preserves every account table")) return 1;
    }

    DatabaseFixture rollback{root / "rollback.sqlite3"};
    if (!check(makeV1(expectedAccount.path, rollback.path)
                   && rollback.database.open(rollback.path.native()) == sqlite::OpenResult::opened
                   && rollback.database.execute(
                       "CREATE VIEW activity_defaults AS SELECT 1 AS singleton;")
                   && rollback.database.inspect_schema() == sqlite::OpenResult::opened
                   && rollback.database.initialize_schema() == sqlite::OpenResult::failed,
               "late migration rollback injection")) return 1;
    sqlite3_int64 applicationAfterRollback = -1;
    sqlite3_int64 createdTables = -1;
    if (!check(query_integer(rollback.database.get(), "PRAGMA user_version;", version) && version == 1
                   && query_integer(rollback.database.get(),
                                    "PRAGMA application_id;",
                                    applicationAfterRollback)
                   && applicationAfterRollback == 0
                   && query_integer(rollback.database.get(),
                                    "SELECT count(*) FROM sqlite_master WHERE type='table' "
                                    "AND name='store_metadata';",
                                    createdTables)
                   && createdTables == 0,
               "rollback retains v1 identity and removes partial tables")) return 1;

    if (!check(malformed_case(root, seedSql, "gap",
                              "UPDATE entitlements SET position=30 WHERE position=18;"),
               "reject gapped positions")
        || !check(malformed_case(root, seedSql, "adjacent",
                                 "UPDATE unlock_flag_runs SET start_index=47 WHERE scope=0 AND position=1;"),
                  "reject adjacent runs")
        || !check(malformed_case(root, seedSql, "zero",
                                 "UPDATE unlock_objective_values SET value=0 WHERE scope=0 AND position=0;"),
                  "reject noncanonical zero value")
        || !check(malformed_case(root, seedSql, "out-of-order",
                                 "UPDATE unlock_objective_values SET slot=100 WHERE scope=0 AND position=0;"),
                  "reject out-of-order sparse values")
        || !check(malformed_case(root, seedSql, "ownership",
                                 "UPDATE entitlements SET ownership=9 WHERE position=0;"),
                  "reject entitlement enum")
        || !check(malformed_case(root, seedSql, "arrival",
                                 "UPDATE activity_arrival_overrides SET bubble=NULL,spawn_set_hash=NULL "
                                 "WHERE position=0;"),
                  "reject empty arrival override")
        || !check(malformed_case(root, seedSql, "bounds",
                                 "UPDATE family5_flag_overrides SET slot=23500 WHERE position=0;"),
                  "reject family-5 bounds")
        || !check(malformed_case(root, seedSql, "extra",
                                 "WITH RECURSIVE n(x) AS (VALUES(39) UNION ALL SELECT x+1 FROM n WHERE x<100) "
                                 "INSERT INTO family5_flag_overrides SELECT x,x,2 FROM n;"),
                  "reject bootstrap rows above capacity")
        || !check(malformed_case(root, seedSql, "metadata",
                                 "UPDATE store_metadata SET seed_hash='bad' WHERE singleton=1;"),
                  "reject malformed metadata")) return 1;

    (void)fresh.database.close();
    const fs::path foreignPath = root / "foreign.sqlite3";
    fs::copy_file(root / "fresh.sqlite3", foreignPath, fs::copy_options::overwrite_existing, error);
    sqlite3* raw = nullptr;
    sqlite3_open_v2(foreignPath.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    (void)execute(raw, "PRAGMA journal_mode=DELETE; PRAGMA application_id=1234;");
    sqlite3_close(raw);
    const auto foreignBytes = read_bytes(foreignPath);
    sqlite::Database foreign;
    sqlite::BootstrapSnapshot foreignBootstrap{};
    if (!check(foreign.open(foreignPath.native()) == sqlite::OpenResult::failed && !foreign.is_writable(),
               "foreign application id is read-only")
        || !check(!sqlite::load_bootstrap(foreign, foreignBootstrap)
                      && !sqlite::save_bootstrap(foreign, loaded),
                  "foreign application id blocks bootstrap access")
        || !check(read_bytes(foreignPath) == foreignBytes, "foreign database unchanged")
        || !check(!fs::exists(foreignPath.string() + "-wal")
                      && !fs::exists(foreignPath.string() + "-shm"),
                  "foreign database has no sidecars")) return 1;

    const fs::path newerPath = root / "newer.sqlite3";
    fs::copy_file(root / "fresh.sqlite3", newerPath, fs::copy_options::overwrite_existing, error);
    raw = nullptr;
    sqlite3_open_v2(newerPath.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    (void)execute(raw, "PRAGMA journal_mode=DELETE; PRAGMA user_version=3;");
    sqlite3_close(raw);
    const auto newerBytes = read_bytes(newerPath);
    sqlite::Database newer;
    if (!check(newer.open(newerPath.native()) == sqlite::OpenResult::newer && !newer.is_writable(),
               "newer schema is read-only")
        || !check(read_bytes(newerPath) == newerBytes, "newer database unchanged")
        || !check(!fs::exists(newerPath.string() + "-wal")
                      && !fs::exists(newerPath.string() + "-shm"),
                  "newer database has no sidecars")) return 1;

    std::cout << "sqlite-bootstrap-snapshot-ok\n";
    return 0;
}
