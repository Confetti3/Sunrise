#include "state/persistence/sqlite/state_store.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace sunrise::state;
using Store = sunrise::state::persistence::sqlite::StateStore;
using namespace sunrise::state::persistence::sqlite;

[[nodiscard]] bool check(bool condition, std::string_view message) noexcept {
    if (!condition) {
        std::cerr << "sqlite account snapshot test failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool exec(sqlite3* database, const char* sql) noexcept {
    return database != nullptr
           && sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

[[nodiscard]] std::vector<char> file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool same_item(const account::inventory::Item& left,
                             const account::inventory::Item& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.level == right.level && left.quantity == right.quantity
           && left.mutationSerial == right.mutationSerial && left.flags == right.flags
           && left.sockets.policy == right.sockets.policy
           && left.sockets.plugCount == right.sockets.plugCount
           && left.sockets.plugs == right.sockets.plugs
           && left.movementAbilityEntry == right.movementAbilityEntry
           && left.grenadeAbilityEntry == right.grenadeAbilityEntry
           && left.superAbilityEntry == right.superAbilityEntry
           && left.meleeAbilityEntry == right.meleeAbilityEntry
           && left.classAbilityEntry == right.classAbilityEntry;
}

[[nodiscard]] bool same_character(const CharacterState& left,
                                  const CharacterState& right) noexcept {
    if (left.soid != right.soid || left.selected != right.selected || left.race != right.race
        || left.gender != right.gender || left.characterClass != right.characterClass
        || left.level != right.level || left.accepted != right.accepted
        || left.previewAvailable != right.previewAvailable
        || left.appearanceValue != right.appearanceValue
        || left.lastOrbitedDestination != right.lastOrbitedDestination
        || left.contentBypass != right.contentBypass
        || left.acquiredSubclassAbilityMask != right.acquiredSubclassAbilityMask
        || left.inventory.count != right.inventory.count
        || left.nextInventorySerial != right.nextInventorySerial) {
        return false;
    }
    for (std::size_t index = 0; index < left.equipment.slots.size(); ++index) {
        if (left.equipment.slots[index].has_value() != right.equipment.slots[index].has_value()
            || (left.equipment.slots[index].has_value()
                && !same_item(*left.equipment.slots[index], *right.equipment.slots[index]))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.inventory.count; ++index) {
        if (!same_item(left.inventory.values[index], right.inventory.values[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_settings(const account::settings::AccountSettings& left,
                                 const account::settings::AccountSettings& right) noexcept {
    const auto& a = left.controls;
    const auto& b = right.controls;
    if (a.buttonLayout != b.buttonLayout || a.movementMode != b.movementMode
        || a.controllerLookSensitivity != b.controllerLookSensitivity
        || a.controllerInvertVertical != b.controllerInvertVertical
        || a.controllerAutoLookCentering != b.controllerAutoLookCentering
        || a.controllerVibration != b.controllerVibration
        || a.controllerSwapShoulders != b.controllerSwapShoulders
        || a.controllerInvertHorizontal != b.controllerInvertHorizontal
        || a.mouseLookSensitivity != b.mouseLookSensitivity
        || a.mouseInvertVertical != b.mouseInvertVertical
        || a.mouseInvertHorizontal != b.mouseInvertHorizontal
        || a.unidentifiedToggle != b.unidentifiedToggle
        || a.mouseAimSmoothing != b.mouseAimSmoothing
        || a.adsSensitivityModifier != b.adsSensitivityModifier
        || a.doublePressDelay != b.doublePressDelay) {
        return false;
    }
    const auto& aa = left.audio;
    const auto& ba = right.audio;
    if (aa.voiceOutputMode != ba.voiceOutputMode || aa.teamVoiceChannel != ba.teamVoiceChannel
        || aa.reservedMode != ba.reservedMode || aa.migrationVersion != ba.migrationVersion
        || aa.chatVolume != ba.chatVolume || aa.muteWhenUnfocused != ba.muteWhenUnfocused
        || aa.soundEffectsVolume != ba.soundEffectsVolume
        || aa.dialogueVolume != ba.dialogueVolume || aa.musicVolume != ba.musicVolume) {
        return false;
    }
    const auto& ad = left.display;
    const auto& bd = right.display;
    if (ad.brightness != bd.brightness || ad.showFps != bd.showFps || ad.hdrMode != bd.hdrMode
        || ad.verticalSyncInterval != bd.verticalSyncInterval || ad.fieldOfView != bd.fieldOfView
        || ad.calibrationPrimary != bd.calibrationPrimary
        || ad.calibrationAlpha != bd.calibrationAlpha) {
        return false;
    }
    const auto& ai = left.interface;
    const auto& bi = right.interface;
    if (ai.subtitlesMode != bi.subtitlesMode || ai.colorblindMode != bi.colorblindMode
        || ai.helmetMode != bi.helmetMode || ai.hudOpacity != bi.hudOpacity
        || ai.displayHints != bi.displayHints || ai.backgroundOpacity != bi.backgroundOpacity
        || ai.reticleLocation != bi.reticleLocation || ai.reticleColor != bi.reticleColor
        || ai.textSize != bi.textSize || ai.textColor != bi.textColor
        || ai.textBackgroundStyle != bi.textBackgroundStyle
        || ai.textBackgroundOpacity != bi.textBackgroundOpacity
        || ai.reservedTextMode != bi.reservedTextMode
        || ai.subtitleOptionsEntry != bi.subtitleOptionsEntry) {
        return false;
    }
    const auto& as = left.social;
    const auto& bs = right.social;
    if (as.preferGoodConnection != bs.preferGoodConnection
        || as.textChatMode != bs.textChatMode || as.showRealNames != bs.showRealNames
        || as.clanInviteNotifications != bs.clanInviteNotifications
        || as.profanityFilter != bs.profanityFilter || as.voiceChatEnabled != bs.voiceChatEnabled
        || as.whisperChatMode != bs.whisperChatMode
        || as.teamChatJoinMode != bs.teamChatJoinMode
        || as.localChatJoinMode != bs.localChatJoinMode
        || as.clanChatJoinMode != bs.clanChatJoinMode
        || as.chatAutoHideMode != bs.chatAutoHideMode
        || left.keyBindingSource != right.keyBindingSource
        || left.keyBindings.configured != right.keyBindings.configured
        || left.configured != right.configured) {
        return false;
    }
    for (std::size_t index = 0; index < left.keyBindings.values.size(); ++index) {
        const auto& lb = left.keyBindings.values[index];
        const auto& rb = right.keyBindings.values[index];
        if (lb.primary != rb.primary || lb.secondary != rb.secondary) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_account(const AccountState& left, const AccountState& right) noexcept {
    if (left.primarySoid != right.primarySoid
        || left.dismantleRewardCount != right.dismantleRewardCount
        || left.profileItemCount != right.profileItemCount
        || left.characterCount != right.characterCount
        || !same_settings(left.settings, right.settings)) {
        return false;
    }
    for (std::size_t index = 0; index < left.dismantleRewardCount; ++index) {
        const auto& a = left.dismantleRewards[index];
        const auto& b = right.dismantleRewards[index];
        if (a.definitionHash != b.definitionHash || a.quantity != b.quantity
            || a.tierMask != b.tierMask || a.classMask != b.classMask
            || a.masterwork != b.masterwork) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.profileItemCount; ++index) {
        const auto& a = left.profileItems[index];
        const auto& b = right.profileItems[index];
        if (a.instanceSoid != b.instanceSoid || a.definitionHash != b.definitionHash
            || a.quantity != b.quantity || a.mutationSerial != b.mutationSerial) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.characterCount; ++index) {
        if (!same_character(left.characters[index], right.characters[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] AccountState fixture() {
    AccountState value{};
    value.primarySoid = 0xF123456789ABCDEFULL;
    value.dismantleRewardCount = 1;
    value.dismantleRewards[0] = {0xFEDCBA98U,
                                 3,
                                 0x02,
                                 0x01,
                                 DismantleMasterworkFilter::masterworked};
    value.profileItemCount = 2;
    value.profileItems[0] = {0xF000000000000001ULL, 0x90000000U, 44, 7};
    value.profileItems[1] = {0, 0xFFFFFFFFU, 1, 8};
    value.characterCount = 2;

    CharacterState& first = value.characters[0];
    first.soid = 0x8123456789ABCDEFULL;
    first.selected = true;
    first.race = CharacterRace::exo;
    first.gender = CharacterGender::female;
    first.characterClass = CharacterClass::warlock;
    first.level = 40;
    first.accepted = true;
    first.previewAvailable = true;
    first.appearanceValue = 1.5F;
    first.lastOrbitedDestination = 0xF0000000U;
    first.contentBypass = true;
    first.acquiredSubclassAbilityMask = ~std::uint64_t{0};
    first.nextInventorySerial = 50;

    account::inventory::Item equipped{};
    equipped.instanceSoid = 0x9000000000000001ULL;
    equipped.definitionHash = 0xE0000001U;
    equipped.level = 1;
    equipped.quantity = 1;
    equipped.mutationSerial = 12;
    equipped.flags = 0xF0000000U;
    equipped.sockets.policy = account::inventory::SocketPolicy::authored;
    equipped.sockets.plugCount = 3;
    equipped.sockets.plugs[0] = 0xFFFFFFFFU;
    equipped.sockets.plugs[1].reset();
    equipped.sockets.plugs[2] = 5U;
    equipped.movementAbilityEntry = 5;
    equipped.grenadeAbilityEntry = 8;
    equipped.superAbilityEntry = 13;
    equipped.meleeAbilityEntry = 12;
    equipped.classAbilityEntry = 3;
    first.equipment.slots[static_cast<std::size_t>(account::inventory::EquipmentSlot::subclass)] =
        equipped;

    account::inventory::Item inventory = equipped;
    inventory.instanceSoid = 0x9000000000000002ULL;
    inventory.definitionHash = 0xE0000002U;
    inventory.sockets.plugs = {};
    inventory.sockets.plugCount = 1;
    inventory.sockets.plugs[0].reset();
    first.inventory.values[0] = inventory;
    first.inventory.count = 1;

    CharacterState& second = value.characters[1];
    second.soid = 2;
    second.level = 20;
    second.accepted = true;
    second.previewAvailable = true;
    second.race = CharacterRace::awoken;
    second.gender = CharacterGender::female;
    second.characterClass = CharacterClass::hunter;
    second.appearanceValue = -2.25F;
    second.lastOrbitedDestination = 7;
    second.acquiredSubclassAbilityMask = 0x8000000000000000ULL;
    second.nextInventorySerial = 3;

    auto& controls = value.settings.controls;
    controls.buttonLayout = 5;
    controls.movementMode = 3;
    controls.controllerLookSensitivity = 9;
    controls.controllerInvertVertical = true;
    controls.controllerAutoLookCentering = true;
    controls.controllerVibration = true;
    controls.controllerSwapShoulders = true;
    controls.controllerInvertHorizontal = true;
    controls.mouseLookSensitivity = 100;
    controls.mouseInvertVertical = true;
    controls.mouseInvertHorizontal = true;
    controls.unidentifiedToggle = true;
    controls.mouseAimSmoothing = true;
    controls.adsSensitivityModifier = 1.5F;
    controls.doublePressDelay = 4;
    auto& audio = value.settings.audio;
    audio.voiceOutputMode = 2;
    audio.teamVoiceChannel = 1;
    audio.reservedMode = 1;
    audio.migrationVersion = 8;
    audio.chatVolume = 8;
    audio.muteWhenUnfocused = true;
    audio.soundEffectsVolume = 10;
    audio.dialogueVolume = 9;
    audio.musicVolume = 8;
    auto& display = value.settings.display;
    display.brightness = 6;
    display.showFps = true;
    display.hdrMode = 1;
    display.verticalSyncInterval = 4;
    display.fieldOfView = 105;
    display.calibrationPrimary = 10000.0F;
    display.calibrationAlpha = 0.0F;
    auto& interfaceSettings = value.settings.interface;
    interfaceSettings.subtitlesMode = 2;
    interfaceSettings.colorblindMode = 3;
    interfaceSettings.helmetMode = 1;
    interfaceSettings.hudOpacity = 3;
    interfaceSettings.displayHints = true;
    interfaceSettings.backgroundOpacity = 4;
    interfaceSettings.reticleLocation = 1;
    interfaceSettings.reticleColor = 6;
    interfaceSettings.textSize = 4;
    interfaceSettings.textColor = 3;
    interfaceSettings.textBackgroundStyle = 3;
    interfaceSettings.textBackgroundOpacity = 4;
    auto& social = value.settings.social;
    social.preferGoodConnection = true;
    social.textChatMode = 3;
    social.showRealNames = true;
    social.clanInviteNotifications = true;
    social.profanityFilter = true;
    social.voiceChatEnabled = true;
    social.whisperChatMode = 1;
    social.teamChatJoinMode = 1;
    social.localChatJoinMode = 1;
    social.clanChatJoinMode = 1;
    social.chatAutoHideMode = 1;
    value.settings.keyBindingSource = account::settings::KeyBindingSource::account;
    value.settings.keyBindings.configured = true;
    for (std::size_t index = 0; index < value.settings.keyBindings.values.size(); ++index) {
        auto& binding = value.settings.keyBindings.values[index];
        binding.primary = static_cast<std::uint16_t>(index * 2U + 1U);
        if (index % 3U == 0U) {
            binding.secondary.reset();
        } else {
            binding.secondary = static_cast<std::uint16_t>(index * 2U + 2U);
        }
    }
    value.settings.keyBindings.values[0].primary = static_cast<std::uint16_t>(0xFFFFU);
    value.settings.configured = true;
    return value;
}

[[nodiscard]] bool open_raw(const std::filesystem::path& path, sqlite3*& output) noexcept {
    output = nullptr;
    return sqlite3_open16(path.wstring().c_str(), &output) == SQLITE_OK && output != nullptr;
}

[[nodiscard]] bool mutate_database(const std::filesystem::path& path, const char* sql) noexcept {
    sqlite3* database = nullptr;
    if (!open_raw(path, database)) {
        return false;
    }
    const bool result = exec(database, sql) && sqlite3_close(database) == SQLITE_OK;
    if (!result && database != nullptr) {
        (void)sqlite3_close(database);
    }
    return result;
}

[[nodiscard]] bool reopen(Store& store, const std::filesystem::path& path) noexcept {
    return store.open(path) == OpenResult::opened
           && store.initialize_schema() == OpenResult::opened;
}

[[nodiscard]] bool set_payload_version(sqlite3* database, std::uint32_t version) noexcept {
    sqlite3_stmt* select = nullptr;
    if (sqlite3_prepare_v2(database,
                           "SELECT settings_payload FROM account_state WHERE singleton = 1;",
                           -1,
                           &select,
                           nullptr)
            != SQLITE_OK
        || select == nullptr || sqlite3_step(select) != SQLITE_ROW) {
        (void)sqlite3_finalize(select);
        return false;
    }
    const int size = sqlite3_column_bytes(select, 0);
    const auto* bytes = static_cast<const std::byte*>(sqlite3_column_blob(select, 0));
    if (size < 4 || bytes == nullptr) {
        (void)sqlite3_finalize(select);
        return false;
    }
    std::vector<std::byte> payload(bytes, bytes + size);
    if (sqlite3_finalize(select) != SQLITE_OK) {
        return false;
    }
    payload[0] = static_cast<std::byte>(version & 0xFFU);
    payload[1] = static_cast<std::byte>((version >> 8U) & 0xFFU);
    payload[2] = static_cast<std::byte>((version >> 16U) & 0xFFU);
    payload[3] = static_cast<std::byte>((version >> 24U) & 0xFFU);
    sqlite3_stmt* update = nullptr;
    const bool updated =
        sqlite3_prepare_v2(database,
                           "UPDATE account_state SET settings_payload = ? WHERE singleton = 1;",
                           -1,
                           &update,
                           nullptr)
            == SQLITE_OK
        && update != nullptr
        && sqlite3_bind_blob(update,
                             1,
                             payload.data(),
                             size,
                             SQLITE_TRANSIENT)
               == SQLITE_OK
        && sqlite3_step(update) == SQLITE_DONE;
    return sqlite3_finalize(update) == SQLITE_OK && updated;
}

} // namespace

int main() {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(error) / "sunrise-sqlite-account-snapshot-test";
    if (!check(!error, "resolve temp directory")) {
        return 1;
    }
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (!check(!error, "create temp directory")) {
        return 1;
    }
    const std::filesystem::path path = root / "account.sqlite3";
    const std::filesystem::path wal = path.wstring() + L"-wal";
    const std::filesystem::path shm = path.wstring() + L"-shm";
    const AccountState input = fixture();
    if (!check(account::valid(input), "fixture is production-valid")) {
        return 1;
    }

    Store store;
    AccountState output{};
    Store unopened;
    if (!check(unopened.load(output) == LoadResult::disabled, "pre-open load disabled")
        || !check(!unopened.save(input), "pre-open save disabled")
        || !check(store.open(path) == OpenResult::opened, "open new store")
        || !check(store.initialize_schema() == OpenResult::opened, "initialize schema")
        || !check(store.load(output) == LoadResult::missing, "missing account before first save")
        || !check(store.save(input), "save complete account")
        || !check(store.load(output) == LoadResult::loaded, "load complete account")
        || !check(same_account(input, output), "two-character field roundtrip")) {
        return 1;
    }

    AccountState invalid = input;
    invalid.characters[0].inventory.values[0].level = -1;
    if (!check(!account::valid(invalid), "invalid account rejected by account::valid")
        || !check(!store.save(invalid), "invalid account save rejected")
        || !check(store.load(output) == LoadResult::loaded, "load after invalid save")
        || !check(same_account(input, output), "invalid save preserved prior snapshot")) {
        return 1;
    }

    auto expect_invalid = [&](const char* sql, std::string_view message) {
        store.close();
        return check(mutate_database(path, sql), message)
               && check(reopen(store, path), "reopen after malformed mutation")
               && check(store.load(output) == LoadResult::invalid, "reject malformed snapshot")
               && check(store.save(input), "repair malformed snapshot");
    };
    if (!expect_invalid("UPDATE account_state SET character_count = 3 WHERE singleton = 1;",
                        "mutate root character count")
        || !expect_invalid("UPDATE account_state SET profile_item_count = 702 WHERE singleton = 1;",
                            "mutate root profile count")
        || !expect_invalid("UPDATE account_state SET dismantle_reward_count = 33 "
                           "WHERE singleton = 1;",
                           "mutate root reward count")
        || !expect_invalid("UPDATE characters SET inventory_count = 2 WHERE position = 0;",
                           "mutate per-character inventory count")
        || !expect_invalid("UPDATE character_items SET plug_count = 4 "
                           "WHERE character_position = 0 AND location = 0 AND position = 11;",
                           "mutate item plug count")
        || !expect_invalid("UPDATE profile_items SET position = 2 WHERE account_id = 1 AND position = 0;",
                           "mutate profile position")
        || !expect_invalid("UPDATE characters SET position = 2 WHERE account_id = 1 AND position = 1;",
                           "mutate character position")
        || !expect_invalid("UPDATE character_items SET position = 2 WHERE character_position = 0 "
                           "AND location = 1 AND position = 0;",
                           "mutate inventory position")
        || !expect_invalid("UPDATE characters SET character_class = 99 WHERE position = 0;",
                           "mutate invalid enum")
        || !expect_invalid("UPDATE dismantle_rewards SET tier_mask = 0x99 "
                           "WHERE account_id = 1 AND position = 0;",
                           "mutate invalid range")
        || !expect_invalid("PRAGMA ignore_check_constraints = ON; "
                           "UPDATE characters SET selected = 2 WHERE position = 0;",
                           "mutate invalid boolean")
        || !expect_invalid("UPDATE account_state SET settings_payload = "
                           "substr(settings_payload, 1, 10) WHERE singleton = 1;",
                           "truncate settings payload")
        || !expect_invalid("UPDATE account_state SET settings_payload = settings_payload || X'00' "
                           "WHERE singleton = 1;",
                           "append settings payload byte")) {
        return 1;
    }
    if (!check(mutate_database(path,
                               "DELETE FROM item_plugs WHERE character_position = 0 "
                               "AND location = 0 AND item_position = 11 AND plug_position = 1;"),
               "remove nullable plug row")
        || !check(reopen(store, path), "reopen after missing plug mutation")
        || !check(store.load(output) == LoadResult::invalid, "missing child row rejected")
        || !check(store.save(input), "repair missing child row")) {
        return 1;
    }
    store.close();
    if (!check(mutate_database(path,
                               "INSERT INTO item_plugs (account_id, character_position, location, "
                               "item_position, plug_position, definition_hash) "
                               "VALUES (1, 0, 0, 11, 3, NULL);"),
               "insert extra plug row")
        || !check(reopen(store, path), "reopen after extra plug mutation")
        || !check(store.load(output) == LoadResult::invalid, "extra child row rejected")
        || !check(store.save(input), "repair extra child row")) {
        return 1;
    }
    store.close();
    if (!check(mutate_database(path,
                               "UPDATE dismantle_rewards SET position = 2 WHERE account_id = 1 "
                               "AND position = 0;"),
               "move reward row out of order")
        || !check(reopen(store, path), "reopen after position mutation")
        || !check(store.load(output) == LoadResult::invalid, "out-of-order child row rejected")
        || !check(store.save(input), "repair out-of-order child row")) {
        return 1;
    }

    store.close();
    if (!check(mutate_database(path,
                               "CREATE TRIGGER reject_snapshot_profile "
                               "BEFORE INSERT ON profile_items BEGIN "
                               "SELECT RAISE(ABORT, 'test rollback'); END;"),
               "install rollback trigger")
        || !check(reopen(store, path), "reopen with rollback trigger")
        || !check(!store.save(input), "failed replacement reports failure")
        || !check(store.load(output) == LoadResult::loaded, "rollback leaves readable snapshot")
        || !check(same_account(input, output), "rollback preserves prior snapshot")) {
        return 1;
    }
    store.close();
    if (!check(mutate_database(path, "DROP TRIGGER reject_snapshot_profile;"),
               "remove rollback trigger")
        || !check(reopen(store, path), "reopen after rollback")
        || !check(store.save(input), "save after rollback")) {
        return 1;
    }

    store.close();
    sqlite3* raw = nullptr;
    if (!check(open_raw(path, raw), "open raw database")
        || !check(exec(raw, "PRAGMA journal_mode = DELETE; PRAGMA user_version = 3;"),
                  "mark newer schema")) {
        (void)sqlite3_close(raw);
        return 1;
    }
    if (!check(sqlite3_close(raw) == SQLITE_OK, "close newer schema database")) {
        return 1;
    }
    const std::vector<char> newerSchemaBytes = file_bytes(path);
    if (!check(!std::filesystem::exists(wal) && !std::filesystem::exists(shm),
               "newer schema has no WAL sidecars")) {
        return 1;
    }
    if (!check(store.open(path) == OpenResult::newer, "newer schema is distinct")
        || !check(!store.is_writable(), "newer schema store is read-only")
        || !check(store.inspect_schema() == OpenResult::newer, "inspect newer schema")
        || !check(store.initialize_schema() == OpenResult::newer, "do not initialize newer schema")
        || !check(store.load(output) == LoadResult::schemaNewer, "do not load newer schema")
        || !check(!store.save(input), "do not save newer schema")) {
        return 1;
    }
    store.close();
    if (!check(file_bytes(path) == newerSchemaBytes, "newer schema bytes unchanged")
        || !check(!std::filesystem::exists(wal) && !std::filesystem::exists(shm),
                  "newer schema sidecars remain absent")) {
        return 1;
    }

    if (!check(open_raw(path, raw), "reopen schema for compatibility checks")
        || !check(exec(raw, "PRAGMA user_version = 2;"), "restore schema version")
        || !check(sqlite3_close(raw) == SQLITE_OK, "close restored schema")) {
        return 1;
    }
    if (!check(reopen(store, path), "open format-v1 database")) {
        return 1;
    }
    store.close();
    if (!check(mutate_database(path,
                               "UPDATE account_state SET format_version = 2 "
                               "WHERE singleton = 1;"),
               "mark newer account format")) {
        return 1;
    }
    const std::vector<char> newerFormatBytes = file_bytes(path);
    if (!check(store.open(path) == OpenResult::opened, "open newer account format")
        || !check(store.initialize_schema() == OpenResult::opened, "inspect newer account format")
        || !check(store.load(output) == LoadResult::formatNewer, "reject newer account format")
        || !check(!store.is_writable(), "newer account format disables writes")
        || !check(!store.save(input), "preserve newer account format")) {
        return 1;
    }
    store.close();
    if (!check(file_bytes(path) == newerFormatBytes, "newer account format bytes unchanged")
        || !check(!std::filesystem::exists(wal) && !std::filesystem::exists(shm),
                  "newer account format has no WAL sidecars")) {
        return 1;
    }

    if (!check(open_raw(path, raw), "reopen account format")
        || !check(exec(raw, "UPDATE account_state SET format_version = 1 WHERE singleton = 1;"),
                  "restore account format")
        || !check(set_payload_version(raw, 2), "mark newer settings payload")
        || !check(sqlite3_close(raw) == SQLITE_OK, "close newer payload")) {
        return 1;
    }
    const std::vector<char> newerPayloadBytes = file_bytes(path);
    if (!check(store.open(path) == OpenResult::opened, "open newer settings payload")
        || !check(store.initialize_schema() == OpenResult::opened, "inspect newer settings payload")
        || !check(store.load(output) == LoadResult::formatNewer, "reject newer settings payload")
        || !check(!store.is_writable(), "newer settings payload disables writes")
        || !check(!store.save(input), "preserve newer settings payload")) {
        return 1;
    }
    store.close();
    if (!check(file_bytes(path) == newerPayloadBytes, "newer settings payload bytes unchanged")
        || !check(!std::filesystem::exists(wal) && !std::filesystem::exists(shm),
                  "newer settings payload has no WAL sidecars")) {
        return 1;
    }

    if (!check(open_raw(path, raw), "open final database for integrity checks")) {
        return 1;
    }
    int integrityOk = 0;
    int foreignKeyRows = 0;
    const auto integrityCallback = [](void* value, int columns, char** values, char**) -> int {
        if (columns == 1 && values != nullptr && values[0] != nullptr
            && std::string_view(values[0]) == "ok") {
            *static_cast<int*>(value) = 1;
        }
        return 0;
    };
    const auto rowCallback = [](void* value, int, char**, char**) -> int {
        ++*static_cast<int*>(value);
        return 0;
    };
    const bool integrity = sqlite3_exec(raw,
                                        "PRAGMA integrity_check;",
                                        integrityCallback,
                                        &integrityOk,
                                        nullptr)
                               == SQLITE_OK
                           && sqlite3_exec(raw,
                                           "PRAGMA foreign_key_check;",
                                           rowCallback,
                                           &foreignKeyRows,
                                           nullptr)
                                  == SQLITE_OK;
    const bool closed = sqlite3_close(raw) == SQLITE_OK;
    if (!check(integrity && integrityOk == 1 && foreignKeyRows == 0 && closed,
               "foreign-key and integrity checks")) {
        return 1;
    }
    std::filesystem::remove_all(root, error);
    if (!check(!error, "remove test database")) {
        return 1;
    }
    std::cout << "sqlite-account-snapshot-ok\n";
    return 0;
}
