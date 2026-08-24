#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../account/account_state.h"
#include "database.h"
#include "settings_codec.h"

namespace sunrise::state::persistence::sqlite::detail {
/** Inserts one item row and its exact optional plug prefix. */
[[nodiscard]] inline bool store_item(Statement& itemInsert,
                                     Statement& plugInsert,
                                     std::size_t characterPosition,
                                     int location,
                                     std::size_t position,
                                     const account::inventory::Item& item) noexcept {
    sqlite3_stmt* statement = itemInsert.get();
    if (statement == nullptr || !bind_count(statement, 1, characterPosition)
        || sqlite3_bind_int(statement, 2, location) != SQLITE_OK
        || !bind_count(statement, 3, position) || !bind_u64(statement, 4, item.instanceSoid)
        || sqlite3_bind_int64(statement, 5, item.definitionHash) != SQLITE_OK
        || sqlite3_bind_int64(statement, 6, item.level) != SQLITE_OK
        || sqlite3_bind_int64(statement, 7, item.quantity) != SQLITE_OK
        || sqlite3_bind_int64(statement, 8, item.mutationSerial) != SQLITE_OK
        || sqlite3_bind_int64(statement, 9, item.flags) != SQLITE_OK
        || sqlite3_bind_int(statement, 10, static_cast<int>(item.sockets.policy)) != SQLITE_OK
        || !bind_count(statement, 11, item.sockets.plugCount)
        || sqlite3_bind_int(statement, 12, item.movementAbilityEntry) != SQLITE_OK
        || sqlite3_bind_int(statement, 13, item.grenadeAbilityEntry) != SQLITE_OK
        || sqlite3_bind_int(statement, 14, item.superAbilityEntry) != SQLITE_OK
        || sqlite3_bind_int(statement, 15, item.meleeAbilityEntry) != SQLITE_OK
        || sqlite3_bind_int(statement, 16, item.classAbilityEntry) != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_DONE || !itemInsert.reset()) {
        return false;
    }
    for (std::size_t plugPosition = 0; plugPosition < item.sockets.plugCount;
         ++plugPosition) {
        sqlite3_stmt* plug = plugInsert.get();
        const std::optional<std::uint32_t>& hash = item.sockets.plugs[plugPosition];
        if (plug == nullptr || !bind_count(plug, 1, characterPosition)
            || sqlite3_bind_int(plug, 2, location) != SQLITE_OK
            || !bind_count(plug, 3, position) || !bind_count(plug, 4, plugPosition)
            || (hash.has_value()
                    ? sqlite3_bind_int64(plug, 5, hash.value()) != SQLITE_OK
                    : sqlite3_bind_null(plug, 5) != SQLITE_OK)
            || sqlite3_step(plug) != SQLITE_DONE || !plugInsert.reset()) {
            return false;
        }
    }
    return true;
}

/** Replaces the complete account snapshot in one durable SQLite transaction. */
[[nodiscard]] inline bool save_account(void* module, const AccountState& accountState) noexcept {
    if (module == nullptr) {
        return true;
    }
    if (!account::valid(accountState)) {
        return false;
    }
    Database database;
    if (open_database(module, database) != OpenResult::opened
        || !database.configure_supported_schema()
        || !database.execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    const bool stored = [&]() noexcept {
        if (!database.execute(
                "DELETE FROM item_plugs; DELETE FROM character_items; DELETE FROM characters; "
                "DELETE FROM profile_items; DELETE FROM dismantle_rewards; "
                "DELETE FROM account_state;")) {
            return false;
        }
        SettingsWriter settings;
        if (!encode_settings(accountState.settings, settings)) {
            return false;
        }
        Statement root(database.get(),
                       "INSERT INTO account_state (singleton, format_version, primary_soid, "
                       "dismantle_reward_count, profile_item_count, character_count, "
                       "settings_payload, updated_unix_seconds) VALUES (1, ?, ?, ?, ?, ?, ?, ?);");
        const std::span<const std::byte> payload = settings.bytes();
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (root.get() == nullptr
            || sqlite3_bind_int(root.get(), 1, kAccountFormatVersion) != SQLITE_OK
            || !bind_u64(root.get(), 2, accountState.primarySoid)
            || !bind_count(root.get(), 3, accountState.dismantleRewardCount)
            || !bind_count(root.get(), 4, accountState.profileItemCount)
            || !bind_count(root.get(), 5, accountState.characterCount)
            || sqlite3_bind_blob(root.get(),
                                 6,
                                 payload.data(),
                                 static_cast<int>(payload.size()),
                                 SQLITE_TRANSIENT)
                   != SQLITE_OK
            || sqlite3_bind_int64(root.get(), 7, static_cast<sqlite3_int64>(now))
                   != SQLITE_OK
            || sqlite3_step(root.get()) != SQLITE_DONE) {
            return false;
        }

        Statement rewardInsert(
            database.get(),
            "INSERT INTO dismantle_rewards (account_id, position, definition_hash, quantity, "
            "tier_mask, class_mask, masterwork) VALUES (1, ?, ?, ?, ?, ?, ?);");
        if (rewardInsert.get() == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index < accountState.dismantleRewardCount; ++index) {
            const DismantleRewardPolicy& reward = accountState.dismantleRewards[index];
            sqlite3_stmt* statement = rewardInsert.get();
            if (!bind_count(statement, 1, index)
                || sqlite3_bind_int64(statement, 2, reward.definitionHash) != SQLITE_OK
                || sqlite3_bind_int64(statement, 3, reward.quantity) != SQLITE_OK
                || sqlite3_bind_int(statement, 4, reward.tierMask) != SQLITE_OK
                || sqlite3_bind_int(statement, 5, reward.classMask) != SQLITE_OK
                || sqlite3_bind_int(statement, 6, static_cast<int>(reward.masterwork)) != SQLITE_OK
                || sqlite3_step(statement) != SQLITE_DONE || !rewardInsert.reset()) {
                return false;
            }
        }

        Statement profileInsert(
            database.get(),
            "INSERT INTO profile_items (account_id, position, instance_soid, definition_hash, "
            "quantity, mutation_serial) VALUES (1, ?, ?, ?, ?, ?);");
        if (profileInsert.get() == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
            const account::inventory::ProfileItem& item = accountState.profileItems[index];
            sqlite3_stmt* statement = profileInsert.get();
            if (!bind_count(statement, 1, index) || !bind_u64(statement, 2, item.instanceSoid)
                || sqlite3_bind_int64(statement, 3, item.definitionHash) != SQLITE_OK
                || sqlite3_bind_int64(statement, 4, item.quantity) != SQLITE_OK
                || sqlite3_bind_int64(statement, 5, item.mutationSerial) != SQLITE_OK
                || sqlite3_step(statement) != SQLITE_DONE || !profileInsert.reset()) {
                return false;
            }
        }

        Statement characterInsert(
            database.get(),
            "INSERT INTO characters (account_id, position, soid, selected, race, gender, "
            "character_class, level, accepted, preview_available, appearance_value, "
            "last_orbited_destination, content_bypass, acquired_subclass_ability_mask, "
            "inventory_count, next_inventory_serial) "
            "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
        Statement itemInsert(
            database.get(),
            "INSERT INTO character_items (account_id, character_position, location, position, "
            "instance_soid, definition_hash, item_level, quantity, mutation_serial, flags, "
            "socket_policy, plug_count, movement_ability_entry, grenade_ability_entry, "
            "super_ability_entry, melee_ability_entry, class_ability_entry) "
            "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
        Statement plugInsert(
            database.get(),
            "INSERT INTO item_plugs (account_id, character_position, location, item_position, "
            "plug_position, definition_hash) VALUES (1, ?, ?, ?, ?, ?);");
        if (characterInsert.get() == nullptr || itemInsert.get() == nullptr
            || plugInsert.get() == nullptr) {
            return false;
        }
        for (std::size_t characterIndex = 0; characterIndex < accountState.characterCount;
             ++characterIndex) {
            const CharacterState& character = accountState.characters[characterIndex];
            sqlite3_stmt* statement = characterInsert.get();
            if (!bind_count(statement, 1, characterIndex)
                || !bind_u64(statement, 2, character.soid)
                || !bind_bool(statement, 3, character.selected)
                || sqlite3_bind_int(statement, 4, static_cast<int>(character.race)) != SQLITE_OK
                || sqlite3_bind_int(statement, 5, static_cast<int>(character.gender)) != SQLITE_OK
                || sqlite3_bind_int(statement, 6, static_cast<int>(character.characterClass))
                       != SQLITE_OK
                || sqlite3_bind_int(statement, 7, character.level) != SQLITE_OK
                || !bind_bool(statement, 8, character.accepted)
                || !bind_bool(statement, 9, character.previewAvailable)
                || sqlite3_bind_double(statement, 10, character.appearanceValue) != SQLITE_OK
                || sqlite3_bind_int64(statement, 11, character.lastOrbitedDestination)
                       != SQLITE_OK
                || !bind_bool(statement, 12, character.contentBypass)
                || !bind_u64(statement, 13, character.acquiredSubclassAbilityMask)
                || !bind_count(statement, 14, character.inventory.count)
                || sqlite3_bind_int64(statement, 15, character.nextInventorySerial) != SQLITE_OK
                || sqlite3_step(statement) != SQLITE_DONE || !characterInsert.reset()) {
                return false;
            }
            for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
                const std::optional<account::inventory::Item>& item =
                    character.equipment.slots[slot];
                if (item.has_value()
                    && !store_item(
                        itemInsert,
                        plugInsert,
                        characterIndex,
                        kEquipmentItemLocation,
                        slot,
                        item.value())) {
                    return false;
                }
            }
            for (std::size_t index = 0; index < character.inventory.count; ++index) {
                if (!store_item(itemInsert,
                                plugInsert,
                                characterIndex,
                                kInventoryItemLocation,
                                index,
                                character.inventory.values[index])) {
                    return false;
                }
            }
        }
        return true;
    }();
    if (!stored || !database.execute("COMMIT;")) {
        (void)database.execute("ROLLBACK;");
        return false;
    }
    // Bound sidecar growth on clean shutdown without changing commit success semantics.
    (void)database.execute("PRAGMA wal_checkpoint(TRUNCATE);");
    return true;
}

} // namespace sunrise::state::persistence::sqlite::detail
