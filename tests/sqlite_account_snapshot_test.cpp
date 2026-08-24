#include "state/persistence/sqlite_account_store.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::wstring g_artifactDirectory;
sunrise::state::AccountState g_runtimeAccount;

[[nodiscard]] bool check(bool condition, std::string_view message) noexcept {
    if (!condition) {
        std::cerr << "sqlite persistence test failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool same_settings(
    const sunrise::state::account::settings::AccountSettings& left,
    const sunrise::state::account::settings::AccountSettings& right) noexcept {
    sunrise::state::persistence::sqlite::detail::SettingsWriter encodedLeft;
    sunrise::state::persistence::sqlite::detail::SettingsWriter encodedRight;
    return sunrise::state::persistence::sqlite::detail::encode_settings(left, encodedLeft)
           && sunrise::state::persistence::sqlite::detail::encode_settings(right, encodedRight)
           && std::equal(encodedLeft.bytes().begin(),
                         encodedLeft.bytes().end(),
                         encodedRight.bytes().begin(),
                         encodedRight.bytes().end());
}

[[nodiscard]] bool same_item(const sunrise::state::account::inventory::Item& left,
                             const sunrise::state::account::inventory::Item& right) noexcept {
    return left.instanceSoid == right.instanceSoid
           && left.definitionHash == right.definitionHash && left.level == right.level
           && left.quantity == right.quantity && left.mutationSerial == right.mutationSerial
           && left.flags == right.flags && left.sockets.policy == right.sockets.policy
           && left.sockets.plugCount == right.sockets.plugCount
           && left.sockets.plugs == right.sockets.plugs
           && left.movementAbilityEntry == right.movementAbilityEntry
           && left.grenadeAbilityEntry == right.grenadeAbilityEntry
           && left.superAbilityEntry == right.superAbilityEntry
           && left.meleeAbilityEntry == right.meleeAbilityEntry
           && left.classAbilityEntry == right.classAbilityEntry;
}

[[nodiscard]] bool same_character(const sunrise::state::CharacterState& left,
                                  const sunrise::state::CharacterState& right) noexcept {
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
        const auto& leftItem = left.equipment.slots[index];
        const auto& rightItem = right.equipment.slots[index];
        if (leftItem.has_value() != rightItem.has_value()
            || (leftItem.has_value() && !same_item(*leftItem, *rightItem))) {
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

[[nodiscard]] bool same_account(const sunrise::state::AccountState& left,
                                const sunrise::state::AccountState& right) noexcept {
    if (left.primarySoid != right.primarySoid
        || left.dismantleRewardCount != right.dismantleRewardCount
        || left.profileItemCount != right.profileItemCount
        || left.characterCount != right.characterCount
        || !same_settings(left.settings, right.settings)) {
        return false;
    }
    for (std::size_t index = 0; index < left.dismantleRewardCount; ++index) {
        const auto& leftReward = left.dismantleRewards[index];
        const auto& rightReward = right.dismantleRewards[index];
        if (leftReward.definitionHash != rightReward.definitionHash
            || leftReward.quantity != rightReward.quantity
            || leftReward.tierMask != rightReward.tierMask
            || leftReward.classMask != rightReward.classMask
            || leftReward.masterwork != rightReward.masterwork) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.profileItemCount; ++index) {
        const auto& leftItem = left.profileItems[index];
        const auto& rightItem = right.profileItems[index];
        if (leftItem.instanceSoid != rightItem.instanceSoid
            || leftItem.definitionHash != rightItem.definitionHash
            || leftItem.quantity != rightItem.quantity
            || leftItem.mutationSerial != rightItem.mutationSerial) {
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

[[nodiscard]] bool set_settings_payload_version(sqlite3* database,
                                                std::uint32_t version,
                                                std::size_t minimumSize = 0) noexcept {
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
    if (size < static_cast<int>(sizeof(version)) || bytes == nullptr) {
        (void)sqlite3_finalize(select);
        return false;
    }
    std::vector<std::byte> payload(bytes, bytes + size);
    if (sqlite3_finalize(select) != SQLITE_OK) {
        return false;
    }
    if (payload.size() < minimumSize) {
        payload.resize(minimumSize, std::byte{0});
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
                             static_cast<int>(payload.size()),
                             SQLITE_TRANSIENT)
               == SQLITE_OK
        && sqlite3_step(update) == SQLITE_DONE;
    return sqlite3_finalize(update) == SQLITE_OK && updated;
}

} // namespace

namespace sunrise::core::path {

bool artifact_directory(void*, Buffer& output) noexcept {
    if (g_artifactDirectory.size() >= output.chars.size()) {
        return false;
    }
    output = {};
    std::copy(g_artifactDirectory.cbegin(), g_artifactDirectory.cend(), output.chars.begin());
    output.length = g_artifactDirectory.size();
    output.chars[output.length] = L'\0';
    return true;
}

bool append(Buffer& output, std::wstring_view suffix) noexcept {
    if (output.length + suffix.size() >= output.chars.size()) {
        return false;
    }
    std::copy(suffix.cbegin(), suffix.cend(), output.chars.begin() + output.length);
    output.length += suffix.size();
    output.chars[output.length] = L'\0';
    return true;
}

} // namespace sunrise::core::path

namespace sunrise::core::log {

void write(Channel, Level, std::string_view) noexcept {}

} // namespace sunrise::core::log

namespace sunrise::state {

bool initialize(void*,
                const AccountState& account,
                const activity::defaults::ActivityDefaults&) noexcept {
    g_runtimeAccount = account;
    return true;
}

void shutdown() noexcept {
    g_runtimeAccount = {};
}

AccountState account_snapshot() noexcept {
    return g_runtimeAccount;
}

} // namespace sunrise::state

int main() {
    namespace persistence = sunrise::state::persistence;
    using namespace sunrise::state;

    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(error) / "sunrise-sqlite-persistence-test";
    if (!check(!error, "resolve temporary directory")) {
        return 1;
    }
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (!check(!error, "create temporary directory")) {
        return 1;
    }
    g_artifactDirectory = root.wstring();

    AccountState input{};
    // These deliberately synthetic values exercise field widths without embedding client data.
    input.primarySoid = 0xF123456789ABCDEFULL;
    input.dismantleRewardCount = 1;
    input.dismantleRewards[0].definitionHash = 0xFEDCBA98U;
    input.dismantleRewards[0].quantity = 3;
    input.dismantleRewards[0].tierMask = 0x02;
    input.dismantleRewards[0].classMask = 0x01;
    input.dismantleRewards[0].masterwork = DismantleMasterworkFilter::masterworked;
    input.profileItemCount = 2;
    input.profileItems[0] = {0x5000000000000001ULL, 0x90000000U, 44, 7};
    input.profileItems[1] = {0, 0xFFFFFFFFU, 1, 8};
    input.characterCount = 2;

    CharacterState& first = input.characters[0];
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
    equipped.movementAbilityEntry = 4;
    equipped.grenadeAbilityEntry = 8;
    equipped.superAbilityEntry = 10;
    equipped.meleeAbilityEntry = 12;
    equipped.classAbilityEntry = 2;
    first.equipment.slots[11] = equipped;

    account::inventory::Item inventory = equipped;
    inventory.instanceSoid = 0x9000000000000002ULL;
    inventory.definitionHash = 0xE0000002U;
    inventory.sockets.plugs = {};
    inventory.sockets.plugCount = 1;
    inventory.sockets.plugs[0].reset();
    first.inventory.values[0] = inventory;
    first.inventory.count = 1;

    CharacterState& second = input.characters[1];
    second.soid = 2;
    second.level = 20;
    second.accepted = true;
    second.previewAvailable = true;
    second.race = CharacterRace::human;
    second.gender = CharacterGender::male;
    second.characterClass = CharacterClass::titan;
    second.appearanceValue = -2.25F;
    second.lastOrbitedDestination = 7;
    second.acquiredSubclassAbilityMask = 0x8000000000000000ULL;
    second.nextInventorySerial = 3;

    input.settings.controls.buttonLayout = 5;
    input.settings.controls.movementMode = 3;
    input.settings.controls.controllerLookSensitivity = 9;
    input.settings.controls.controllerInvertVertical = true;
    input.settings.controls.controllerAutoLookCentering = true;
    input.settings.controls.controllerVibration = true;
    input.settings.controls.controllerSwapShoulders = true;
    input.settings.controls.controllerInvertHorizontal = true;
    input.settings.controls.mouseLookSensitivity = 100;
    input.settings.controls.mouseInvertVertical = true;
    input.settings.controls.mouseInvertHorizontal = true;
    input.settings.controls.unidentifiedToggle = true;
    input.settings.controls.mouseAimSmoothing = true;
    input.settings.controls.adsSensitivityModifier = 1.5F;
    input.settings.controls.doublePressDelay = 4;
    input.settings.audio.voiceOutputMode = 2;
    input.settings.audio.teamVoiceChannel = 1;
    input.settings.audio.reservedMode = 1;
    input.settings.audio.migrationVersion = 8;
    input.settings.audio.chatVolume = 8;
    input.settings.audio.muteWhenUnfocused = true;
    input.settings.audio.soundEffectsVolume = 10;
    input.settings.audio.dialogueVolume = 9;
    input.settings.audio.musicVolume = 8;
    input.settings.display.brightness = 6;
    input.settings.display.showFps = true;
    input.settings.display.hdrMode = 1;
    input.settings.display.verticalSyncInterval = 4;
    input.settings.display.fieldOfView = 105;
    input.settings.display.calibrationPrimary = 10000.0F;
    input.settings.display.calibrationAlpha = 0.0F;
    input.settings.interface.subtitlesMode = 2;
    input.settings.interface.colorblindMode = 3;
    input.settings.interface.helmetMode = 1;
    input.settings.interface.hudOpacity = 3;
    input.settings.interface.displayHints = true;
    input.settings.interface.backgroundOpacity = 4;
    input.settings.interface.reticleLocation = 1;
    input.settings.interface.reticleColor = 6;
    input.settings.interface.textSize = 4;
    input.settings.interface.textColor = 3;
    input.settings.interface.textBackgroundStyle = 3;
    input.settings.interface.textBackgroundOpacity = 4;
    input.settings.social.preferGoodConnection = true;
    input.settings.social.textChatMode = 3;
    input.settings.social.showRealNames = true;
    input.settings.social.clanInviteNotifications = true;
    input.settings.social.profanityFilter = true;
    input.settings.social.voiceChatEnabled = true;
    input.settings.social.whisperChatMode = 1;
    input.settings.social.teamChatJoinMode = 1;
    input.settings.social.localChatJoinMode = 1;
    input.settings.social.clanChatJoinMode = 1;
    input.settings.social.chatAutoHideMode = 1;
    input.settings.keyBindingSource = account::settings::KeyBindingSource::account;
    input.settings.keyBindings.configured = true;
    input.settings.keyBindings.values[0].primary = 0xFFFFU;
    input.settings.keyBindings.values[0].secondary.reset();
    input.settings.keyBindings.values.back().primary = 1U;
    input.settings.keyBindings.values.back().secondary = 2U;
    input.settings.configured = true;

    int moduleToken = 0;
    void* module = &moduleToken;
    if (!check(account::valid(input), "production-valid fixture")
        || !check(persistence::sqlite::detail::save_account(module, input), "save account")) {
        return 1;
    }
    AccountState output{};
    if (!check(persistence::sqlite::detail::load_account(module, output)
                   == persistence::sqlite::detail::LoadResult::loaded,
               "load account")
        || !check(same_account(input, output), "complete account round trip")) {
        return 1;
    }
    AccountState invalid = input;
    invalid.characters[0].equipment.slots[11]->level = -1;
    if (!check(!account::valid(invalid), "reject invalid fixture")
        || !check(!persistence::sqlite::detail::save_account(module, invalid),
                  "reject invalid save")
        || !check(persistence::sqlite::detail::load_account(module, output)
                      == persistence::sqlite::detail::LoadResult::loaded,
                  "load after rejected save")
        || !check(same_account(input, output), "rejected save preserves snapshot")) {
        return 1;
    }

    const std::wstring databasePath = (root / "state.sqlite3").wstring();
    sqlite3* database = nullptr;
    if (!check(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK,
               "open database for inspection")) {
        return 1;
    }
    int integrity = 0;
    int foreignKeyViolations = 0;
    int deleteJournal = 0;
    const auto integrityCallback = [](void* value, int columns, char** values, char**) -> int {
        if (columns == 1 && values != nullptr && values[0] != nullptr
            && std::string_view(values[0]) == "ok") {
            *static_cast<int*>(value) = 1;
        }
        return 0;
    };
    const auto rowCountCallback = [](void* value, int, char**, char**) -> int {
        ++*static_cast<int*>(value);
        return 0;
    };
    const auto deleteJournalCallback = [](void* value,
                                          int columns,
                                          char** values,
                                          char**) -> int {
        if (columns == 1 && values != nullptr && values[0] != nullptr
            && std::string_view(values[0]) == "delete") {
            *static_cast<int*>(value) = 1;
        }
        return 0;
    };
    const bool inspected =
        sqlite3_exec(database,
                     "PRAGMA integrity_check;",
                     integrityCallback,
                     &integrity,
                     nullptr)
            == SQLITE_OK
        && integrity == 1
        && sqlite3_exec(database,
                        "PRAGMA foreign_key_check;",
                        rowCountCallback,
                        &foreignKeyViolations,
                        nullptr)
               == SQLITE_OK
        && foreignKeyViolations == 0
        && sqlite3_exec(database,
                        "PRAGMA journal_mode = DELETE;",
                        deleteJournalCallback,
                        &deleteJournal,
                        nullptr)
               == SQLITE_OK
        && deleteJournal == 1
        && sqlite3_exec(database, "PRAGMA user_version = 2;", nullptr, nullptr, nullptr)
               == SQLITE_OK
        && sqlite3_close(database) == SQLITE_OK;
    database = nullptr;
    if (!check(inspected, "integrity and schema version setup")) {
        return 1;
    }
    AccountState ignored{};
    if (!check(persistence::sqlite::detail::load_account(module, ignored)
                   == persistence::sqlite::detail::LoadResult::schemaNewer,
               "reject newer schema")) {
        return 1;
    }

    if (!check(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK,
               "reopen database")) {
        return 1;
    }
    int preservedJournal = 0;
    const bool preservedSchema =
        sqlite3_exec(database,
                     "PRAGMA journal_mode;",
                     deleteJournalCallback,
                     &preservedJournal,
                     nullptr)
            == SQLITE_OK
        && preservedJournal == 1
        && sqlite3_exec(database, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr)
               == SQLITE_OK;
    if (!check(preservedSchema, "newer schema remains in delete journal mode")) {
        (void)sqlite3_close(database);
        return 1;
    }
    const bool damaged =
        sqlite3_exec(database,
                     "DELETE FROM item_plugs WHERE character_position = 0 "
                     "AND location = 0 AND item_position = 11 AND plug_position = 1;",
                     nullptr,
                     nullptr,
                     nullptr)
            == SQLITE_OK
        && sqlite3_close(database) == SQLITE_OK;
    database = nullptr;
    if (!check(damaged, "damage child row")) {
        return 1;
    }
    if (!check(persistence::sqlite::detail::load_account(module, ignored)
                   == persistence::sqlite::detail::LoadResult::invalid,
               "reject incomplete child rows")
        || !check(persistence::sqlite::detail::save_account(module, input), "repair snapshot")
        || !check(persistence::sqlite::detail::load_account(module, ignored)
                      == persistence::sqlite::detail::LoadResult::loaded,
                  "load repaired snapshot")) {
        return 1;
    }

    if (!check(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK,
               "open account-format database")) {
        return 1;
    }
    const bool newerAccountFormat =
        sqlite3_exec(database,
                     "UPDATE account_state SET format_version = 2 WHERE singleton = 1;",
                     nullptr,
                     nullptr,
                     nullptr)
            == SQLITE_OK
        && sqlite3_close(database) == SQLITE_OK;
    database = nullptr;
    activity::defaults::ActivityDefaults defaults{};
    if (!check(newerAccountFormat, "set newer account format")
        || !check(persistence::initialize(module, input, defaults),
                  "fallback from newer account format")) {
        return 1;
    }
    persistence::shutdown();
    if (!check(persistence::sqlite::detail::load_account(module, ignored)
                   == persistence::sqlite::detail::LoadResult::formatNewer,
               "preserve newer account format")) {
        return 1;
    }
    if (!check(sqlite3_open16(databasePath.c_str(), &database) == SQLITE_OK,
               "restore account format")) {
        return 1;
    }
    const bool restoredAccountFormat =
        sqlite3_exec(database,
                     "UPDATE account_state SET format_version = 1 WHERE singleton = 1;",
                     nullptr,
                     nullptr,
                     nullptr)
            == SQLITE_OK
        && set_settings_payload_version(database,
                                        2,
                                        persistence::sqlite::detail::kSettingsPayloadCapacity + 1)
        && sqlite3_close(database) == SQLITE_OK;
    database = nullptr;
    if (!check(restoredAccountFormat, "set newer settings format")
        || !check(persistence::initialize(module, input, defaults),
                  "fallback from newer settings format")) {
        return 1;
    }
    persistence::shutdown();
    if (!check(persistence::sqlite::detail::load_account(module, ignored)
                   == persistence::sqlite::detail::LoadResult::formatNewer,
               "preserve newer settings format")) {
        return 1;
    }
    if (!check(persistence::sqlite::detail::save_account(module, input),
               "restore settings format")
        || !check(persistence::sqlite::detail::load_account(module, output)
                      == persistence::sqlite::detail::LoadResult::loaded,
                  "load after compatibility checks")
        || !check(same_account(input, output), "compatibility checks preserve account")) {
        return 1;
    }

    std::filesystem::remove_all(root, error);
    if (!check(!error, "remove temporary directory")) {
        return 1;
    }
    std::cout << "sqlite-account-snapshot-ok\n";
    return 0;
}
