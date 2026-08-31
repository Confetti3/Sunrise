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
