#include "schema.h"

#include "database.h"

namespace sunrise::state::persistence::sqlite {
namespace {

inline constexpr const char* kSchemaV1 = R"sunrise_sql(
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
PRAGMA user_version = 1;
)sunrise_sql";

inline constexpr const char* kMigrationV1ToV2 = R"sunrise_sql(
CREATE TABLE store_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    database_uuid TEXT NOT NULL,
    seed_revision INTEGER NOT NULL CHECK (seed_revision >= 0),
    seed_hash TEXT NOT NULL,
    content_build INTEGER NOT NULL CHECK (content_build >= 0),
    image_timestamp INTEGER NOT NULL CHECK (image_timestamp BETWEEN 0 AND 4294967295),
    image_size INTEGER NOT NULL CHECK (image_size BETWEEN 0 AND 4294967295),
    configured_equipment_hash INTEGER NOT NULL,
    legacy_source_version INTEGER NOT NULL CHECK (legacy_source_version >= 0),
    legacy_source_hash TEXT NOT NULL,
    account_origin INTEGER NOT NULL CHECK (account_origin BETWEEN 0 AND 2),
    bootstrap_origin INTEGER NOT NULL CHECK (bootstrap_origin BETWEEN 0 AND 2),
    compaction_state INTEGER NOT NULL CHECK (compaction_state BETWEEN 0 AND 2)
);
CREATE TABLE entitlements (
    position INTEGER PRIMARY KEY CHECK (position >= 0),
    name TEXT NOT NULL,
    ownership INTEGER NOT NULL CHECK (ownership BETWEEN 0 AND 2)
);
CREATE TABLE unlock_flag_runs (
    scope INTEGER NOT NULL CHECK (scope BETWEEN 0 AND 3),
    position INTEGER NOT NULL CHECK (position >= 0),
    start_index INTEGER NOT NULL CHECK (start_index >= 0),
    run_length INTEGER NOT NULL CHECK (run_length > 0),
    PRIMARY KEY (scope, position)
);
CREATE TABLE unlock_objective_values (
    scope INTEGER NOT NULL CHECK (scope BETWEEN 0 AND 1),
    position INTEGER NOT NULL CHECK (position >= 0),
    slot INTEGER NOT NULL CHECK (slot >= 0),
    value INTEGER NOT NULL CHECK (value BETWEEN -2147483648 AND 2147483647),
    PRIMARY KEY (scope, position)
);
CREATE TABLE unlock_progressions (
    scope INTEGER NOT NULL CHECK (scope BETWEEN 0 AND 1),
    position INTEGER NOT NULL CHECK (position >= 0),
    definition_index INTEGER NOT NULL CHECK (definition_index >= 0),
    lane0 INTEGER NOT NULL CHECK (lane0 BETWEEN -2147483648 AND 2147483647),
    lane1 INTEGER NOT NULL CHECK (lane1 BETWEEN -2147483648 AND 2147483647),
    lane2 INTEGER NOT NULL CHECK (lane2 BETWEEN -2147483648 AND 2147483647),
    PRIMARY KEY (scope, position)
);
CREATE TABLE family5_flag_overrides (
    position INTEGER PRIMARY KEY CHECK (position >= 0),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 0 AND 23499),
    value INTEGER NOT NULL CHECK (value BETWEEN 0 AND 2)
);
CREATE TABLE family5_value_overrides (
    position INTEGER PRIMARY KEY CHECK (position >= 0),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 0 AND 15499),
    value INTEGER NOT NULL CHECK (value BETWEEN -2147483648 AND 2147483647)
);
CREATE TABLE activity_defaults (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    reason INTEGER NOT NULL CHECK (reason BETWEEN -1 AND 14),
    previous_activity_index INTEGER NOT NULL CHECK (previous_activity_index BETWEEN -1 AND 4094),
    activity_index INTEGER NOT NULL CHECK (activity_index BETWEEN 0 AND 4094),
    package_name TEXT NOT NULL,
    bubble_count INTEGER NOT NULL CHECK (bubble_count BETWEEN 1 AND 64),
    stateful_bubble_mask INTEGER NOT NULL,
    initial_slice_set INTEGER NOT NULL CHECK (initial_slice_set BETWEEN 0 AND 511),
    spawn_set_hash INTEGER NOT NULL CHECK (spawn_set_hash BETWEEN 0 AND 4294967295),
    roster_key_from_identity INTEGER NOT NULL CHECK (roster_key_from_identity IN (0, 1)),
    roster_key_on_all_slots INTEGER NOT NULL CHECK (roster_key_on_all_slots IN (0, 1))
);
CREATE TABLE activity_arrival_overrides (
    position INTEGER PRIMARY KEY CHECK (position >= 0),
    package_name TEXT NOT NULL,
    bubble INTEGER CHECK (bubble BETWEEN 0 AND 63),
    spawn_set_hash INTEGER CHECK (spawn_set_hash BETWEEN 0 AND 4294967295),
    CHECK (bubble IS NOT NULL OR spawn_set_hash IS NOT NULL)
);
PRAGMA application_id = 1398099538;
PRAGMA user_version = 2;
)sunrise_sql";

} // namespace

std::string_view schema_v1_sql() noexcept { return std::string_view(kSchemaV1).substr(1); }

std::string_view migration_v1_to_v2_sql() noexcept {
    return std::string_view(kMigrationV1ToV2).substr(1);
}

bool initialize_state_schema(Database& database) noexcept {
    if (!database.is_writable() || !database.healthy()
        || database.schema_version() < 0 || database.schema_version() > kSchemaVersion) {
        return false;
    }
    if (database.schema_version() == kSchemaVersion) {
        return true;
    }

    bool committed = false;
    {
        Transaction transaction(database);
        bool complete = transaction.active();
        if (complete && database.schema_version() == 0) {
            complete = database.execute(kSchemaV1);
        }
        if (complete) {
            complete = database.execute(kMigrationV1ToV2);
        }
        if (complete) {
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
