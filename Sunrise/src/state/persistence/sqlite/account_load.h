#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../account/account_state.h"
#include "database.h"
#include "settings_codec.h"

namespace sunrise::state::persistence::sqlite::detail {
/** Outcome of attempting to restore one account. */
enum class LoadResult {
    disabled,
    missing,
    loaded,
    schemaNewer,
    formatNewer,
    invalid,
};

/** Loads one exact plug prefix for an already-read item row. */
[[nodiscard]] inline bool load_plugs(sqlite3* database,
                                     std::size_t characterPosition,
                                     int location,
                                     std::size_t itemPosition,
                                     account::inventory::Item& item) noexcept {
    Statement statement(
        database,
        "SELECT plug_position, definition_hash FROM item_plugs "
        "WHERE account_id = 1 AND character_position = ? AND location = ? "
        "AND item_position = ? ORDER BY plug_position;");
    if (statement.get() == nullptr
        || !bind_count(statement.get(), 1, characterPosition)
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

/** Loads every equipment and inventory item belonging to one character row. */
[[nodiscard]] inline bool load_items(sqlite3* database,
                                     std::size_t characterPosition,
                                     std::size_t expectedInventoryCount,
                                     CharacterState& character) noexcept {
    Statement statement(
        database,
        "SELECT location, position, instance_soid, definition_hash, item_level, quantity, "
        "mutation_serial, flags, socket_policy, plug_count, movement_ability_entry, "
        "grenade_ability_entry, super_ability_entry, melee_ability_entry, class_ability_entry "
        "FROM character_items WHERE account_id = 1 AND character_position = ? "
        "ORDER BY location, position;");
    if (statement.get() == nullptr
        || !bind_count(statement.get(), 1, characterPosition)) {
        return false;
    }
    std::array<bool, account::inventory::kEquipmentSlotCount> equipmentSeen{};
    std::size_t inventorySeen = 0;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const int location = sqlite3_column_int(statement.get(), 0);
        const std::size_t capacity =
            location == kEquipmentItemLocation ? account::inventory::kEquipmentSlotCount
                                               : account::inventory::kCharacterItemCapacity;
        std::size_t position = 0;
        account::inventory::Item item{};
        std::uint8_t socketPolicy = 0;
        if ((location != kEquipmentItemLocation && location != kInventoryItemLocation)
            || !column_count(statement.get(), 1, capacity, position) || position >= capacity) {
            return false;
        }
        item.instanceSoid = unsigned_integer(sqlite3_column_int64(statement.get(), 2));
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

/** Loads and validates one complete account row and every normalized child row. */
[[nodiscard]] inline LoadResult load_account(void* module, AccountState& output) noexcept {
    output = {};
    if (module == nullptr) {
        return LoadResult::disabled;
    }
    Database database;
    const OpenResult opened = open_database(module, database);
    if (opened == OpenResult::newer) {
        return LoadResult::schemaNewer;
    }
    if (opened != OpenResult::opened) {
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
    AccountState candidate{};
    if (rootStep != SQLITE_ROW) {
        return LoadResult::invalid;
    }
    const sqlite3_int64 accountFormat = sqlite3_column_int64(root.get(), 0);
    if (accountFormat > kAccountFormatVersion) {
        return LoadResult::formatNewer;
    }
    if (accountFormat != kAccountFormatVersion) {
        return LoadResult::invalid;
    }
    candidate.primarySoid = unsigned_integer(sqlite3_column_int64(root.get(), 1));
    if (!column_count(root.get(),
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
    if (payloadSize <= 0 || payload == nullptr) {
        return LoadResult::invalid;
    }
    const SettingsDecodeResult settings =
        decode_settings({payload, static_cast<std::size_t>(payloadSize)}, candidate.settings);
    if (settings == SettingsDecodeResult::incompatible) {
        return LoadResult::formatNewer;
    }
    if (payloadSize > static_cast<int>(kSettingsPayloadCapacity)
        || settings != SettingsDecodeResult::decoded
        || sqlite3_step(root.get()) != SQLITE_DONE) {
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
            || position != profileSeen || profileSeen >= candidate.profileItemCount) {
            return LoadResult::invalid;
        }
        item.instanceSoid = unsigned_integer(sqlite3_column_int64(profiles.get(), 1));
        if (!column_u32(profiles.get(), 2, item.definitionHash)
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
            || position != characterSeen || characterSeen >= candidate.characterCount) {
            return LoadResult::invalid;
        }
        character.soid = unsigned_integer(sqlite3_column_int64(characters.get(), 1));
        if (!column_bool(characters.get(), 2, character.selected)
            || !column_u8(characters.get(), 3, race)
            || !column_u8(characters.get(), 4, gender)
            || !column_u8(characters.get(), 5, characterClass)
            || !column_u8(characters.get(), 6, character.level)
            || !column_bool(characters.get(), 7, character.accepted)
            || !column_bool(characters.get(), 8, character.previewAvailable)
            || !column_float(characters.get(), 9, character.appearanceValue)
            || !column_u32(characters.get(), 10, character.lastOrbitedDestination)
            || !column_bool(characters.get(), 11, character.contentBypass)
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
        character.acquiredSubclassAbilityMask =
            unsigned_integer(sqlite3_column_int64(characters.get(), 12));
        candidate.characters[characterSeen++] = character;
    }
    if (step != SQLITE_DONE || characterSeen != candidate.characterCount) {
        return LoadResult::invalid;
    }
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (!load_items(database.get(),
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

} // namespace sunrise::state::persistence::sqlite::detail
