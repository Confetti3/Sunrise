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
