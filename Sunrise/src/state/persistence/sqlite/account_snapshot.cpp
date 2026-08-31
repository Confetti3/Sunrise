#include "account_snapshot.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace sunrise::state::persistence::sqlite {
namespace {

[[nodiscard]] bool valid_foreign_keys(Database& database) noexcept {
    Statement statement(database.get(), "PRAGMA foreign_key_check;");
    if (statement.get() == nullptr) {
        return false;
    }
    const int step = sqlite3_step(statement.get());
    return step == SQLITE_DONE;
}

[[nodiscard]] bool load_plugs(Database& database,
                              std::size_t characterPosition,
                              int location,
                              std::size_t itemPosition,
                              account::inventory::Item& item) noexcept {
    Statement statement(
        database.get(),
        "SELECT plug_position, definition_hash FROM item_plugs "
        "WHERE account_id = 1 AND character_position = ? AND location = ? "
        "AND item_position = ? ORDER BY plug_position;");
    if (statement.get() == nullptr || !bind_count(statement.get(), 1, characterPosition)
        || sqlite3_bind_int(statement.get(), 2, location) != SQLITE_OK
        || !bind_count(statement.get(), 3, itemPosition)) {
        return false;
    }
    std::size_t seen = 0;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        std::size_t position = 0;
        if (!column_count(statement.get(), 0, account::inventory::kPlugCapacity, position)
            || position != seen || seen >= item.sockets.plugCount) {
            return false;
        }
        if (sqlite3_column_type(statement.get(), 1) == SQLITE_NULL) {
            item.sockets.plugs[seen].reset();
        } else {
            std::uint32_t definitionHash = 0;
            if (!column_u32(statement.get(), 1, definitionHash)) {
                return false;
            }
            item.sockets.plugs[seen] = definitionHash;
        }
        ++seen;
    }
    return step == SQLITE_DONE && seen == item.sockets.plugCount;
}

[[nodiscard]] bool load_items(Database& database,
                              std::size_t characterPosition,
                              std::size_t expectedInventoryCount,
                              CharacterState& character) noexcept {
    Statement statement(
        database.get(),
        "SELECT location, position, instance_soid, definition_hash, item_level, quantity, "
        "mutation_serial, flags, socket_policy, plug_count, movement_ability_entry, "
        "grenade_ability_entry, super_ability_entry, melee_ability_entry, class_ability_entry "
        "FROM character_items WHERE account_id = 1 AND character_position = ? "
        "ORDER BY location, position;");
    if (statement.get() == nullptr || !bind_count(statement.get(), 1, characterPosition)) {
        return false;
    }
    std::array<bool, account::inventory::kEquipmentSlotCount> equipmentSeen{};
    std::size_t inventorySeen = 0;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
            return false;
        }
        const int location = sqlite3_column_int(statement.get(), 0);
        const std::size_t capacity =
            location == kEquipmentItemLocation ? account::inventory::kEquipmentSlotCount
                                               : account::inventory::kCharacterItemCapacity;
        std::size_t position = 0;
        account::inventory::Item item{};
        std::uint8_t socketPolicy = 0;
        if ((location != kEquipmentItemLocation && location != kInventoryItemLocation)
            || !column_count(statement.get(), 1, capacity, position) || position >= capacity
            || !column_u64(statement.get(), 2, item.instanceSoid)) {
            return false;
        }
        if (!column_u32(statement.get(), 3, item.definitionHash)
            || !column_i32(statement.get(), 4, item.level)
            || !column_i32(statement.get(), 5, item.quantity)
            || !column_i32(statement.get(), 6, item.mutationSerial)
            || !column_u32(statement.get(), 7, item.flags)
            || !column_u8(statement.get(), 8, socketPolicy)
            || !column_count(statement.get(),
                             9,
                             account::inventory::kPlugCapacity,
                             item.sockets.plugCount)
            || !column_u8(statement.get(), 10, item.movementAbilityEntry)
            || !column_u8(statement.get(), 11, item.grenadeAbilityEntry)
            || !column_u8(statement.get(), 12, item.superAbilityEntry)
            || !column_u8(statement.get(), 13, item.meleeAbilityEntry)
            || !column_u8(statement.get(), 14, item.classAbilityEntry)) {
            return false;
        }
        item.sockets.policy = static_cast<account::inventory::SocketPolicy>(socketPolicy);
        if (!load_plugs(database, characterPosition, location, position, item)) {
            return false;
        }
        if (location == kEquipmentItemLocation) {
            if (equipmentSeen[position]) {
                return false;
            }
            equipmentSeen[position] = true;
            character.equipment.slots[position] = item;
        } else {
            if (position != inventorySeen || inventorySeen >= expectedInventoryCount) {
                return false;
            }
            character.inventory.values[inventorySeen++] = item;
        }
    }
    character.inventory.count = inventorySeen;
    return step == SQLITE_DONE && inventorySeen == expectedInventoryCount;
}

[[nodiscard]] bool store_item(Statement& itemInsert,
                              Statement& plugInsert,
                              std::size_t characterPosition,
                              int location,
                              std::size_t position,
                              const account::inventory::Item& item) noexcept {
    sqlite3_stmt* statement = itemInsert.get();
    if (statement == nullptr || !bind_count(statement, 1, characterPosition)
        || sqlite3_bind_int(statement, 2, location) != SQLITE_OK
        || !bind_count(statement, 3, position) || !bind_u64(statement, 4, item.instanceSoid)
        || !bind_u32(statement, 5, item.definitionHash) || !bind_i32(statement, 6, item.level)
        || !bind_i32(statement, 7, item.quantity)
        || !bind_i32(statement, 8, item.mutationSerial)
        || !bind_u32(statement, 9, item.flags)
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
            || (hash.has_value() ? !bind_u32(plug, 5, hash.value())
                                  : sqlite3_bind_null(plug, 5) != SQLITE_OK)
            || sqlite3_step(plug) != SQLITE_DONE || !plugInsert.reset()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool store_format_is_supported(Database& database) noexcept {
    Statement statement(database.get(),
                        "SELECT format_version, settings_payload FROM account_state "
                        "WHERE singleton = 1;");
    if (statement.get() == nullptr) {
        return false;
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return true;
    }
    if (step != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
        return false;
    }
    const sqlite3_int64 format = sqlite3_column_int64(statement.get(), 0);
    if (format > kAccountFormatVersion) {
        return false;
    }
    if (format < 0) {
        return false;
    }
    const int size = sqlite3_column_bytes(statement.get(), 1);
    const auto* payload = static_cast<const std::byte*>(sqlite3_column_blob(statement.get(), 1));
    if (size < 0 || (size != 0 && payload == nullptr)) {
        return false;
    }
    if (size >= 4) {
        const std::uint32_t version = std::to_integer<std::uint8_t>(payload[0])
                                      | (static_cast<std::uint32_t>(
                                             std::to_integer<std::uint8_t>(payload[1]))
                                         << 8U)
                                      | (static_cast<std::uint32_t>(
                                             std::to_integer<std::uint8_t>(payload[2]))
                                         << 16U)
                                      | (static_cast<std::uint32_t>(
                                             std::to_integer<std::uint8_t>(payload[3]))
                                         << 24U);
        if (version > kSettingsPayloadVersion) {
            return false;
        }
    }
    return true;
}

} // namespace

LoadResult load_account(Database& database, AccountState& output) noexcept {
    output = {};
    if (!database.is_open()) {
        return LoadResult::disabled;
    }
    const OpenResult schema = database.inspect_schema();
    if (schema == OpenResult::newer) {
        return LoadResult::schemaNewer;
    }
    if (schema != OpenResult::opened || database.schema_version() != kSchemaVersion
        || !valid_foreign_keys(database)) {
        return LoadResult::invalid;
    }

    Statement root(database.get(),
                   "SELECT format_version, primary_soid, dismantle_reward_count, "
                   "profile_item_count, character_count, settings_payload "
                   "FROM account_state WHERE singleton = 1;");
    if (root.get() == nullptr) {
        return LoadResult::invalid;
    }
    const int rootStep = sqlite3_step(root.get());
    if (rootStep == SQLITE_DONE) {
        return LoadResult::missing;
    }
    if (rootStep != SQLITE_ROW) {
        return LoadResult::invalid;
    }
    std::int32_t accountFormat = 0;
    if (!column_i32(root.get(), 0, accountFormat)) {
        return LoadResult::invalid;
    }
    if (accountFormat > kAccountFormatVersion) {
        return LoadResult::formatNewer;
    }
    if (accountFormat != kAccountFormatVersion) {
        return LoadResult::invalid;
    }
    AccountState candidate{};
    if (!column_u64(root.get(), 1, candidate.primarySoid)
        || !column_count(root.get(),
                         2,
                         candidate.dismantleRewards.size(),
                         candidate.dismantleRewardCount)
        || !column_count(root.get(),
                         3,
                         candidate.profileItems.size(),
                         candidate.profileItemCount)
        || !column_count(root.get(),
                         4,
                         candidate.characters.size(),
                         candidate.characterCount)) {
        return LoadResult::invalid;
    }
    const int payloadSize = sqlite3_column_bytes(root.get(), 5);
    const auto* payload = static_cast<const std::byte*>(sqlite3_column_blob(root.get(), 5));
    if (sqlite3_column_type(root.get(), 5) != SQLITE_BLOB || payloadSize <= 0
        || payload == nullptr) {
        return LoadResult::invalid;
    }
    const SettingsDecodeResult settings =
        decode_settings({payload, static_cast<std::size_t>(payloadSize)}, candidate.settings);
    if (settings == SettingsDecodeResult::incompatible) {
        return LoadResult::formatNewer;
    }
    if (payloadSize > static_cast<int>(kSettingsPayloadCapacity)
        || settings != SettingsDecodeResult::decoded || sqlite3_step(root.get()) != SQLITE_DONE) {
        return LoadResult::invalid;
    }

    Statement rewards(
        database.get(),
        "SELECT position, definition_hash, quantity, tier_mask, class_mask, masterwork "
        "FROM dismantle_rewards WHERE account_id = 1 ORDER BY position;");
    if (rewards.get() == nullptr) {
        return LoadResult::invalid;
    }
    std::size_t rewardSeen = 0;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(rewards.get())) == SQLITE_ROW) {
        std::size_t position = 0;
        DismantleRewardPolicy reward{};
        std::uint8_t masterwork = 0;
        if (!column_count(rewards.get(), 0, candidate.dismantleRewards.size(), position)
            || position != rewardSeen || rewardSeen >= candidate.dismantleRewardCount
            || !column_u32(rewards.get(), 1, reward.definitionHash)
            || !column_i32(rewards.get(), 2, reward.quantity)
            || !column_u8(rewards.get(), 3, reward.tierMask)
            || !column_u8(rewards.get(), 4, reward.classMask)
            || !column_u8(rewards.get(), 5, masterwork)) {
            return LoadResult::invalid;
        }
        reward.masterwork = static_cast<DismantleMasterworkFilter>(masterwork);
        candidate.dismantleRewards[rewardSeen++] = reward;
    }
    if (step != SQLITE_DONE || rewardSeen != candidate.dismantleRewardCount) {
        return LoadResult::invalid;
    }

    Statement profiles(database.get(),
                       "SELECT position, instance_soid, definition_hash, quantity, "
                       "mutation_serial FROM profile_items WHERE account_id = 1 "
                       "ORDER BY position;");
    if (profiles.get() == nullptr) {
        return LoadResult::invalid;
    }
    std::size_t profileSeen = 0;
    while ((step = sqlite3_step(profiles.get())) == SQLITE_ROW) {
        std::size_t position = 0;
        account::inventory::ProfileItem item{};
        if (!column_count(profiles.get(), 0, candidate.profileItems.size(), position)
            || position != profileSeen || profileSeen >= candidate.profileItemCount
            || !column_u64(profiles.get(), 1, item.instanceSoid)
            || !column_u32(profiles.get(), 2, item.definitionHash)
            || !column_i32(profiles.get(), 3, item.quantity)
            || !column_i32(profiles.get(), 4, item.mutationSerial)) {
            return LoadResult::invalid;
        }
        candidate.profileItems[profileSeen++] = item;
    }
    if (step != SQLITE_DONE || profileSeen != candidate.profileItemCount) {
        return LoadResult::invalid;
    }

    Statement characters(
        database.get(),
        "SELECT position, soid, selected, race, gender, character_class, level, accepted, "
        "preview_available, appearance_value, last_orbited_destination, content_bypass, "
        "acquired_subclass_ability_mask, inventory_count, next_inventory_serial "
        "FROM characters WHERE account_id = 1 ORDER BY position;");
    if (characters.get() == nullptr) {
        return LoadResult::invalid;
    }
    std::array<std::size_t, kCharacterCapacity> inventoryCounts{};
    std::size_t characterSeen = 0;
    while ((step = sqlite3_step(characters.get())) == SQLITE_ROW) {
        std::size_t position = 0;
        CharacterState character{};
        std::uint8_t race = 0;
        std::uint8_t gender = 0;
        std::uint8_t characterClass = 0;
        if (!column_count(characters.get(), 0, candidate.characters.size(), position)
            || position != characterSeen || characterSeen >= candidate.characterCount
            || !column_u64(characters.get(), 1, character.soid)
            || !column_bool(characters.get(), 2, character.selected)
            || !column_u8(characters.get(), 3, race)
            || !column_u8(characters.get(), 4, gender)
            || !column_u8(characters.get(), 5, characterClass)
            || !column_u8(characters.get(), 6, character.level)
            || !column_bool(characters.get(), 7, character.accepted)
            || !column_bool(characters.get(), 8, character.previewAvailable)
            || !column_float(characters.get(), 9, character.appearanceValue)
            || !column_u32(characters.get(), 10, character.lastOrbitedDestination)
            || !column_bool(characters.get(), 11, character.contentBypass)
            || !column_u64(characters.get(), 12, character.acquiredSubclassAbilityMask)
            || !column_count(characters.get(),
                             13,
                             account::inventory::kCharacterItemCapacity,
                             inventoryCounts[characterSeen])
            || !column_u32(characters.get(), 14, character.nextInventorySerial)) {
            return LoadResult::invalid;
        }
        character.race = static_cast<CharacterRace>(race);
        character.gender = static_cast<CharacterGender>(gender);
        character.characterClass = static_cast<CharacterClass>(characterClass);
        candidate.characters[characterSeen++] = character;
    }
    if (step != SQLITE_DONE || characterSeen != candidate.characterCount) {
        return LoadResult::invalid;
    }
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (!load_items(database,
                        index,
                        inventoryCounts[index],
                        candidate.characters[index])) {
            return LoadResult::invalid;
        }
    }
    if (!account::valid(candidate)) {
        return LoadResult::invalid;
    }
    output = candidate;
    return LoadResult::loaded;
}

bool save_account(Database& database, const AccountState& account) noexcept {
    if (!database.is_open() || !database.is_writable()
        || database.inspect_schema() != OpenResult::opened
        || database.schema_version() != kSchemaVersion || !account::valid(account)
        || !store_format_is_supported(database) || !database.enable_wal()) {
        return false;
    }
    Transaction transaction(database);
    if (!transaction.active()) {
        return false;
    }
    const bool stored = [&]() noexcept {
        if (!database.execute("DELETE FROM item_plugs; DELETE FROM character_items; "
                              "DELETE FROM characters; DELETE FROM profile_items; "
                              "DELETE FROM dismantle_rewards; DELETE FROM account_state;")) {
            return false;
        }
        SettingsWriter settings;
        if (!encode_settings(account.settings, settings)) {
            return false;
        }
        const std::span<const std::byte> payload = settings.bytes();
        Statement root(
            database.get(),
            "INSERT INTO account_state (singleton, format_version, primary_soid, "
            "dismantle_reward_count, profile_item_count, character_count, settings_payload, "
            "updated_unix_seconds) VALUES (1, ?, ?, ?, ?, ?, ?, ?);");
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (root.get() == nullptr || !bind_i32(root.get(), 1, kAccountFormatVersion)
            || !bind_u64(root.get(), 2, account.primarySoid)
            || !bind_count(root.get(), 3, account.dismantleRewardCount)
            || !bind_count(root.get(), 4, account.profileItemCount)
            || !bind_count(root.get(), 5, account.characterCount)
            || payload.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
            || sqlite3_bind_blob(root.get(),
                                 6,
                                 payload.data(),
                                 static_cast<int>(payload.size()),
                                 SQLITE_TRANSIENT)
                   != SQLITE_OK
            || sqlite3_bind_int64(root.get(), 7, static_cast<sqlite3_int64>(now)) != SQLITE_OK
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
        for (std::size_t index = 0; index < account.dismantleRewardCount; ++index) {
            const DismantleRewardPolicy& reward = account.dismantleRewards[index];
            sqlite3_stmt* statement = rewardInsert.get();
            if (!bind_count(statement, 1, index) || !bind_u32(statement, 2, reward.definitionHash)
                || !bind_i32(statement, 3, reward.quantity)
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
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const account::inventory::ProfileItem& item = account.profileItems[index];
            sqlite3_stmt* statement = profileInsert.get();
            if (!bind_count(statement, 1, index) || !bind_u64(statement, 2, item.instanceSoid)
                || !bind_u32(statement, 3, item.definitionHash)
                || !bind_i32(statement, 4, item.quantity)
                || !bind_i32(statement, 5, item.mutationSerial)
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
        for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
             ++characterIndex) {
            const CharacterState& character = account.characters[characterIndex];
            sqlite3_stmt* statement = characterInsert.get();
            if (!bind_count(statement, 1, characterIndex) || !bind_u64(statement, 2, character.soid)
                || !bind_bool(statement, 3, character.selected)
                || sqlite3_bind_int(statement, 4, static_cast<int>(character.race)) != SQLITE_OK
                || sqlite3_bind_int(statement, 5, static_cast<int>(character.gender)) != SQLITE_OK
                || sqlite3_bind_int(statement, 6, static_cast<int>(character.characterClass))
                       != SQLITE_OK
                || sqlite3_bind_int(statement, 7, character.level) != SQLITE_OK
                || !bind_bool(statement, 8, character.accepted)
                || !bind_bool(statement, 9, character.previewAvailable)
                || sqlite3_bind_double(statement, 10, character.appearanceValue) != SQLITE_OK
                || !bind_u32(statement, 11, character.lastOrbitedDestination)
                || !bind_bool(statement, 12, character.contentBypass)
                || !bind_u64(statement, 13, character.acquiredSubclassAbilityMask)
                || !bind_count(statement, 14, character.inventory.count)
                || !bind_u32(statement, 15, character.nextInventorySerial)
                || sqlite3_step(statement) != SQLITE_DONE || !characterInsert.reset()) {
                return false;
            }
            for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
                const std::optional<account::inventory::Item>& item =
                    character.equipment.slots[slot];
                if (item.has_value()
                    && !store_item(itemInsert,
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
    if (!stored || !transaction.commit()) {
        (void)transaction.rollback();
        return false;
    }
    // Keep clean test runs and normal shutdowns from retaining an unbounded WAL sidecar.
    (void)database.execute("PRAGMA wal_checkpoint(TRUNCATE);");
    return true;
}

} // namespace sunrise::state::persistence::sqlite
