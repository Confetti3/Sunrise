#include "web_service_actions.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode1821.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode2400.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode701/opcode701_codec.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/progression/season_pass_reward_catalog.h"
#include "../../state/progression/seasonal_experience.h"
#include "../../state/record_claims/record_claims.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::web_service {

namespace {

/** Socket kind the shader model occupies, which is the only kind a shader swap may target. */
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;
/** Index stored when no definition resolves. The catalog is u16-indexed, so this cannot be one. */
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();

template <std::size_t Size>
void write_warning(const std::array<char, Size>& line, int count) noexcept {
    if (count <= 0) {
        return;
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     {line.data(), (std::min)(static_cast<std::size_t>(count), line.size() - 1U)});
}

/** Prepares one rank-one class package without changing account State. */
[[nodiscard]] bool
prepare_premium_class_package(const state::progression::season_pass::PremiumClassPackage& package,
                              state::PendingDirectItemBundle& mutation) noexcept {
    std::array<std::uint16_t, state::progression::season_pass::kPremiumPackageItemCount>
        itemIndices{};
    for (std::size_t index = 0; index < package.items.size(); ++index) {
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(package.items[index], definition)) {
            return false;
        }
        itemIndices[index] = definition.definitionIndex;
    }
    return state::prepare_direct_item_bundle(package.hash, itemIndices, mutation);
}

/** Expands the manifest-authored destination package into all nine material stacks. */
[[nodiscard]] bool
prepare_destination_resource_bundle(state::PendingSeasonPassReward& grant) noexcept {
    namespace pass = state::progression::season_pass;
    std::array<state::DirectRecordReward, pass::kDestinationResourceHashes.size()> rewards{};
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(pass::kDestinationResourceHashes[index],
                                                          definition)) {
            return false;
        }
        rewards[index] = {definition.definitionIndex, pass::kDestinationResourceQuantity};
    }
    auto& mutation = grant.grant.emplace<state::PendingRecordRewardGrant>();
    return state::prepare_record_reward_grant(rewards, {}, mutation);
}

/** Chooses one installed weapon or selected-class armour item from an auto-decrypting engram. */
template <std::size_t Size>
[[nodiscard]] std::span<const std::uint32_t>
class_armour_pool(state::CharacterClass characterClass,
                  const std::array<std::uint32_t, Size>& titan,
                  const std::array<std::uint32_t, Size>& hunter,
                  const std::array<std::uint32_t, Size>& warlock) noexcept {
    switch (characterClass) {
    case state::CharacterClass::hunter:
        return hunter;
    case state::CharacterClass::warlock:
        return warlock;
    case state::CharacterClass::titan:
    default:
        return titan;
    }
}

[[nodiscard]] bool choose_engram_reward(std::uint32_t engramHash,
                                        std::uint16_t& itemIndex) noexcept {
    namespace pass = state::progression::season_pass;
    std::span<const std::uint32_t> weapons;
    std::span<const std::uint32_t> armour;
    const state::AccountState account = state::account_snapshot();
    const state::CharacterState* character = nullptr;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            character = &account.characters[index];
            break;
        }
    }
    if (character == nullptr) {
        return false;
    }

    if (engramHash == pass::kLegendaryEngramHash) {
        weapons = pass::kLegendaryEngramWeapons;
        armour = class_armour_pool(character->characterClass,
                                   pass::kLegendaryTitanArmour,
                                   pass::kLegendaryHunterArmour,
                                   pass::kLegendaryWarlockArmour);
    } else if (engramHash == pass::kExoticEngramHash) {
        weapons = pass::kExoticEngramWeapons;
        armour = class_armour_pool(character->characterClass,
                                   pass::kExoticTitanArmour,
                                   pass::kExoticHunterArmour,
                                   pass::kExoticWarlockArmour);
    } else {
        return false;
    }

    std::array<std::byte, sizeof(std::uint32_t)> randomBytes{};
    if (!middleware::crypto::random::fill(randomBytes)) {
        return false;
    }
    const std::size_t count = weapons.size() + armour.size();
    const std::size_t first = middleware::encoding::read_u32_le(randomBytes) % count;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t selected = (first + offset) % count;
        const std::uint32_t hash = selected < weapons.size()
                                       ? weapons[selected]
                                       : armour[selected - weapons.size()];
        state::build_data::items::Definition definition{};
        if (state::build_data::find_item_definition_hash(hash, definition)) {
            itemIndex = definition.definitionIndex;
            return true;
        }
    }
    return false;
}

struct SeasonArmourRoll {
    std::uint16_t rewardIndex{};
    std::uint32_t itemHash{};
    std::array<std::uint32_t, 4> statPlugs{};
};

/** Exact rolls shown by the free, early premium, and high-stat pass tiles. */
constexpr std::array<SeasonArmourRoll, 36> kSeasonArmourRolls{{
    // Hunter
    {29, 3097544525U, {1232924472U, 3898228147U, 1177742666U, 1322519294U}},
    {106, 3097544525U, {2191854732U, 3198072315U, 1322519294U, 597281635U}},
    {139, 3097544525U, {3072193746U, 772656086U, 2692705068U, 1300719726U}},
    {3, 3750210364U, {1232924472U, 3898228147U, 1177742666U, 757147114U}},
    {78, 3750210364U, {2120002858U, 1526511067U, 1277668557U, 3014984195U}},
    {111, 3750210364U, {2600679334U, 3955940376U, 3026009912U, 115819390U}},
    {24, 2930001572U, {1232924472U, 2518640481U, 1177742666U, 4237052128U}},
    {97, 2930001572U, {2191854732U, 2440285137U, 1322519294U, 597281635U}},
    {134, 2930001572U, {4286256569U, 2148570570U, 594234536U, 2664898188U}},
    {10, 3136019014U, {2191854732U, 2236091344U, 1177742666U, 2596709069U}},
    {83, 3136019014U, {3407231789U, 1798836563U, 1322519294U, 3203727595U}},
    {120, 3136019014U, {3346214146U, 2399358832U, 594234536U, 2701881372U}},
    // Titan
    {30, 1214477175U, {1232924472U, 3898228147U, 1177742666U, 1322519294U}},
    {107, 1214477175U, {479726201U, 176934377U, 1322519294U, 597281635U}},
    {140, 1214477175U, {2868325782U, 2387165386U, 2692705068U, 1300719726U}},
    {4, 287888126U, {1232924472U, 3898228147U, 1177742666U, 757147114U}},
    {79, 287888126U, {2120002858U, 1369119565U, 1277668557U, 3014984195U}},
    {112, 287888126U, {2148570570U, 2148570570U, 3026009912U, 115819390U}},
    {25, 1585947570U, {1232924472U, 2518640481U, 1177742666U, 4237052128U}},
    {98, 1585947570U, {1232924472U, 1533918171U, 1322519294U, 597281635U}},
    {135, 1585947570U, {2868325782U, 1087628722U, 594234536U, 2664898188U}},
    {11, 1131831128U, {2191854732U, 2236091344U, 1177742666U, 2596709069U}},
    {84, 1131831128U, {1232924472U, 4002613733U, 1322519294U, 3203727595U}},
    {121, 1131831128U, {2387165386U, 2148570570U, 594234536U, 2701881372U}},
    // Warlock
    {31, 1173249516U, {1232924472U, 3898228147U, 1177742666U, 1322519294U}},
    {108, 1173249516U, {2191854732U, 2433764085U, 1322519294U, 597281635U}},
    {141, 1173249516U, {2868325782U, 3072193746U, 2692705068U, 1300719726U}},
    {5, 327547301U, {1232924472U, 3898228147U, 1177742666U, 757147114U}},
    {80, 327547301U, {1232924472U, 3554741641U, 1277668557U, 3014984195U}},
    {113, 327547301U, {4248662490U, 1281324436U, 3026009912U, 115819390U}},
    {26, 119457531U, {1232924472U, 2518640481U, 1177742666U, 4237052128U}},
    {99, 119457531U, {1232924472U, 1858911761U, 1322519294U, 597281635U}},
    {136, 119457531U, {3198618292U, 643846500U, 594234536U, 2664898188U}},
    {12, 674876967U, {2191854732U, 2236091344U, 1177742666U, 2596709069U}},
    {85, 674876967U, {2576224072U, 1534361879U, 1322519294U, 3203727595U}},
    {122, 674876967U, {643846500U, 4248662490U, 594234536U, 2701881372U}},
}};

[[nodiscard]] constexpr bool season_armour_rolls_valid() noexcept {
    for (const SeasonArmourRoll& roll : kSeasonArmourRolls) {
        const auto* reward = state::progression::season_pass::find(roll.rewardIndex);
        if (reward == nullptr || reward->itemHash != roll.itemHash) {
            return false;
        }
    }
    return true;
}

static_assert(season_armour_rolls_valid());

/** Replaces native stat plugs with the fixed roll represented by one known pass tile. */
[[nodiscard]] bool apply_season_armour_roll(std::uint16_t rewardIndex,
                                            state::PendingItemAcquisition& mutation) noexcept {
    const auto roll = std::find_if(kSeasonArmourRolls.begin(),
                                   kSeasonArmourRolls.end(),
                                   [rewardIndex](const SeasonArmourRoll& candidate) {
                                       return candidate.rewardIndex == rewardIndex;
                                   });
    if (roll == kSeasonArmourRolls.end()) {
        return true;
    }
    if (mutation.inventoryIndex >= mutation.afterCharacter.inventory.count) {
        return false;
    }
    auto& acquired = mutation.afterCharacter.inventory.values[mutation.inventoryIndex];
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    if (acquired.definitionHash != mutation.acquiredDefinitionHash
        || acquired.definitionHash != roll->itemHash
        || !state::build_data::find_item_definition_hash(acquired.definitionHash, item)
        || !state::build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex || detail.ordinarySocketCount <= 9U
        || detail.ordinarySocketCount > acquired.sockets.plugs.size()) {
        return false;
    }

    state::account::inventory::Sockets sockets{};
    sockets.policy = state::account::inventory::SocketPolicy::authored;
    sockets.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < sockets.plugCount; ++lane) {
        const std::uint16_t plugIndex = detail.initialPlugIndices[lane];
        if (plugIndex == state::build_data::items::details::kUnavailableItemIndex) {
            continue;
        }
        state::build_data::items::Definition plug{};
        if (!state::build_data::find_item_definition_index(plugIndex, plug)) {
            return false;
        }
        sockets.plugs[lane] = plug.definitionHash;
    }
    for (std::size_t index = 0; index < roll->statPlugs.size(); ++index) {
        sockets.plugs[6U + index] = roll->statPlugs[index];
    }
    acquired.sockets = sockets;
    return state::account::inventory::valid(acquired.sockets);
}

} // namespace

/** Decodes and prepares one sparse account-settings writeback without publishing State. */
state::SettingsUpdateDisposition mutate_settings(const middleware::web_service::Message& message,
                                                 Outcome& outcome) noexcept {
    namespace opcode701 = middleware::web_service::messages::opcode701;

    opcode701::Request request{};
    if (!opcode701::parse_request(message, request)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws701 stage=prepare result=rejected reason=parse");
        return state::SettingsUpdateDisposition::rejected;
    }

    state::PendingSettingsUpdate mutation{};
    const state::SettingsUpdateDisposition disposition =
        state::prepare_settings_update(request.settings, mutation);
    if (disposition == state::SettingsUpdateDisposition::preparedMutation) {
        if (emplace_mutation<state::PendingSettingsUpdate>(outcome, mutation) == nullptr) {
            return state::SettingsUpdateDisposition::rejected;
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=ws701 stage=prepare result=ready");
        return disposition;
    }
    if (disposition == state::SettingsUpdateDisposition::acceptedNoChange) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=ws701 stage=prepare result=no_change");
        return disposition;
    }

    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=ws701 stage=prepare result=rejected reason=validation");
    return state::SettingsUpdateDisposition::rejected;
}

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key once the selection has moved in State.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(picked.characterSoid, changed)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterChanged = changed;
    outcome.selectedCharacterSoid = picked.characterSoid;
}

/** Reads the shared opcode-403/404 SOID descriptor through its codec. */
[[nodiscard]] bool parse_equipment_instance(const middleware::web_service::Message& message,
                                            std::uint64_t& instanceSoid) noexcept {
    middleware::web_service::messages::opcode403::Request request{};
    const bool parsed =
        middleware::web_service::messages::opcode403::parse_request(message, request);
    instanceSoid = request.instanceSoid;
    return parsed;
}

/** Prepares one opcode-403/404 equipment mutation without publishing State early. */
void mutate_equipment(const middleware::web_service::Message& message,
                      bool unequip,
                      Outcome& outcome) noexcept {
    std::uint64_t requestedInstanceSoid = 0;
    if (!parse_equipment_instance(message, requestedInstanceSoid)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=equipment stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingEquipmentSwap>(outcome);
    if (mutation == nullptr) {
        return;
    }
    const bool prepared = unequip
                              ? state::prepare_equipment_unequip(requestedInstanceSoid, *mutation)
                              : state::prepare_equipment_swap(requestedInstanceSoid, *mutation);
    if (!prepared) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=equipment stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one exact selected-character opcode-801 subclass node selection. */
void mutate_subclass_selection(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode801::Request request{};
    if (!middleware::web_service::messages::opcode801::parse_request(message, request)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws801 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingSubclassSelection>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_subclass_selection(
            request.subclassInstanceSoid, request.socketEntry, *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws801 stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one exact selected-character opcode-903 socket selection. */
void mutate_socket_plug(const middleware::web_service::Message& message,
                        Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode903::Request request{};
    if (!middleware::web_service::messages::opcode903::parse_request(message, request)
        || !request.hasInstance || request.instanceSoid == 0 || request.hasTargetDefinition
        || !request.hasPlugDefinition
        || request.socketIndex >= state::account::inventory::kPlugCapacity) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws903 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingSocketPlug>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_socket_plug(request.instanceSoid,
                                    static_cast<std::uint8_t>(request.socketIndex),
                                    request.plugDefinitionIndex,
                                    *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws903 stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one character-location opcode-1901 socket selection. */
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept {
    namespace opcode1901 = middleware::web_service::messages::opcode1901;
    opcode1901::Request request{};
    const bool parsed = opcode1901::parse_request(message, request);
    // One transaction can stage one socket mutation, so understood batch requests are refused.
    const opcode1901::Replacement& replacement = request.replacements.front();
    if (!parsed || request.replacementCount != 1
        || replacement.modelSocketKind != kEquippedShaderModelSocketKind
        || replacement.auxiliary != 0
        || replacement.socketIndex >= state::account::inventory::kPlugCapacity
        || request.instanceIdentityToken == 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws1901 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingSocketPlug>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_character_selector_socket_plug(
            request.instanceIdentityToken,
            static_cast<std::uint8_t>(replacement.socketIndex),
            replacement.plugDefinitionIndex,
            *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws1901 stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one complete accumulated item-state value from opcode 406. */
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode406::Request request{};
    if (!middleware::web_service::messages::opcode406::parse_request(message, request)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws406 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingItemState>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_item_state(
            request.instanceSoid, request.definitionIndex, request.flags, *mutation)) {
        clear_mutation(outcome);
        return;
    }
}

/** Reports an opcode-402 validation failure. */
void report_item_dismantle(const middleware::web_service::Message& message,
                           std::string_view reason,
                           std::uint64_t instanceSoid,
                           std::uint32_t definitionIndex,
                           std::uint32_t definitionHash,
                           std::uint32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws402 stage=prepare result=fail reason=%.*s transaction=%u payload_bytes=%zu "
        "instance=0x%llX definition_index=%u definition_hash=0x%08X quantity=%u",
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        static_cast<unsigned long long>(instanceSoid),
        definitionIndex,
        definitionHash,
        quantity);
    write_warning(line, count);
}

/** Prepares the exact fixed-width opcode-402 Character-inventory removal request. */
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode402::Request request{};
    if (!middleware::web_service::messages::opcode402::parse_request(message, request)) {
        report_item_dismantle(
            message, "payload_bits", request.instanceSoid, request.definitionIndex, 0, 0);
        return;
    }
    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint16_t definitionIndex = request.definitionIndex;
    // The codec owns the value; this alias keeps the dismantle checks below readable.
    constexpr std::uint32_t kSingleQuantity =
        middleware::web_service::messages::opcode402::kSingleQuantity;

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)) {
        report_item_dismantle(
            message, "definition", instanceSoid, definitionIndex, 0, kSingleQuantity);
        return;
    }
    auto* mutation = emplace_mutation<state::PendingItemDismantle>(outcome);
    if (mutation == nullptr) {
        report_item_dismantle(message,
                              "storage",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (!state::prepare_item_dismantle(instanceSoid, *mutation)) {
        clear_mutation(outcome);
        report_item_dismantle(message,
                              "state",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (mutation->dismantledItem.definitionHash != definition.definitionHash
        || mutation->dismantledItem.quantity != static_cast<std::int32_t>(kSingleQuantity)) {
        clear_mutation(outcome);
        report_item_dismantle(message,
                              "identity",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
}

/** Reports an opcode-1801 claim failure. */
void report_record_claim(const middleware::web_service::Message& message,
                         std::string_view reason,
                         std::uint32_t recordIndex,
                         std::uint32_t completionFlagIndex,
                         std::uint32_t scoreValue) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1801 stage=claim result=fail reason=%.*s transaction=%u payload_bytes=%zu "
        "record_index=%u completion_flag_index=%u score=%u total_score=%u claims=%zu",
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        recordIndex,
        completionFlagIndex,
        scoreValue,
        state::record_claims::total_score(),
        state::record_claims::count());
    write_warning(line, count);
}

/** Reports an opcode-1820 validation failure. */
void report_item_acquisition(const middleware::web_service::Message& message,
                             std::string_view reason,
                             std::uint32_t collectibleIndex,
                             std::uint32_t itemDefinitionIndex,
                             std::uint32_t definitionHash,
                             std::uint64_t instanceSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1820 stage=prepare result=fail reason=%.*s transaction=%u payload_bytes=%zu "
        "collectible_index=%u item_definition_index=%u definition_hash=0x%08X instance=0x%llX",
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        collectibleIndex,
        itemDefinitionIndex,
        definitionHash,
        static_cast<unsigned long long>(instanceSoid));
    write_warning(line, count);
}

/** Prepares the exact three-byte opcode-1820 Collections item request. */
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode1820::Request request{};
    if (!middleware::web_service::messages::opcode1820::parse_request(message, request)) {
        report_item_acquisition(message,
                                "payload_bits",
                                kUnavailableDefinitionIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    const std::uint16_t collectibleIndex = request.collectibleIndex;
    std::uint16_t itemDefinitionIndex = 0;
    if (!state::build_data::find_collectible_item_definition_index(collectibleIndex,
                                                                   itemDefinitionIndex)) {
        report_item_acquisition(
            message, "collectible_definition", collectibleIndex, kUnavailableDefinitionIndex, 0, 0);
        return;
    }

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        report_item_acquisition(
            message, "item_definition", collectibleIndex, itemDefinitionIndex, 0, 0);
        return;
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        report_item_acquisition(message,
                                "item_detail_or_bucket",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            report_item_acquisition(message,
                                    "profile_item_instanced",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        auto* mutation = emplace_mutation<state::PendingProfileItemAcquisition>(outcome);
        if (mutation == nullptr) {
            report_item_acquisition(message,
                                    "storage",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        if (!state::prepare_profile_item_acquisition(
                collectibleIndex, definition.definitionHash, *mutation)) {
            clear_mutation(outcome);
            report_item_acquisition(message,
                                    "profile_state",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        return;
    }
    if (bucket.arraySelector != bucket_domain::ArraySelector::character) {
        report_item_acquisition(message,
                                "unsupported_inventory_array",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    auto* mutation = emplace_mutation<state::PendingItemAcquisition>(outcome);
    if (mutation == nullptr) {
        report_item_acquisition(message,
                                "storage",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    if (!state::prepare_item_acquisition(collectibleIndex, definition.definitionHash, *mutation)) {
        clear_mutation(outcome);
        report_item_acquisition(
            message, "state", collectibleIndex, itemDefinitionIndex, definition.definitionHash, 0);
        return;
    }
}

/** Reports a record-reward preparation failure. */
void report_record_reward(const middleware::web_service::Message& message,
                          std::string_view reason,
                          std::uint32_t recordIndex,
                          std::uint32_t itemIndex,
                          std::int32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ws1801 stage=reward result=fail reason=%.*s transaction=%u "
                                    "record_index=%u item_index=%u quantity=%d",
                                    static_cast<int>(reason.size()),
                                    reason.data(),
                                    static_cast<unsigned>(message.transactionId),
                                    recordIndex,
                                    itemIndex,
                                    quantity);
    write_warning(line, count);
}

/** Prepares one direct Season-pass item, returning a failure reason or null. */
[[nodiscard]] const char* prepare_direct_reward(std::uint16_t itemIndex,
                                                std::int32_t quantity,
                                                state::PendingSeasonPassReward& grant) noexcept {
    if (quantity <= 0) {
        return "quantity";
    }
    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemIndex, definition)) {
        return "item_definition";
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemIndex, detail)
        || detail.definitionIndex != itemIndex || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        return "item_detail_or_bucket";
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            return "profile_item_instanced";
        }
        auto& mutation = grant.grant.emplace<state::PendingProfileItemAcquisition>();
        return state::prepare_profile_item_acquisition_for_item(itemIndex, quantity, mutation)
                   ? nullptr
                   : "profile_state";
    }
    if (bucket.arraySelector == bucket_domain::ArraySelector::character) {
        if (quantity != 1) {
            return "character_item_quantity";
        }
        auto& mutation = grant.grant.emplace<state::PendingItemAcquisition>();
        if (!state::prepare_item_acquisition_for_item(itemIndex, mutation)) {
            return "state";
        }
        mutation.profileChanged = false;
        return nullptr;
    }
    return "unsupported_inventory_array";
}

enum class RecordRewardPreparation : std::uint8_t {
    absent,
    prepared,
    failed,
};

/** Prepares a settings override or, when absent, the generated manifest reward. */
[[nodiscard]] RecordRewardPreparation
prepare_record_reward(const middleware::web_service::Message& message,
                      std::uint16_t recordIndex,
                      std::uint32_t recordHash,
                      const state::record_claims::PendingClaim& claim,
                      Outcome& outcome) noexcept {
    std::array<state::DirectRecordReward, state::kRecordRewardGrantCapacity> rewards{};
    std::size_t rewardCount = 0;
    state::RecordRewardPolicy override{};
    if (state::account::find_record_reward(state::account_snapshot(), recordIndex, override)) {
        rewards[0] = {override.itemIndex, override.quantity};
        rewardCount = 1;
    } else {
        std::array<state::build_data::records::rewards::ResolvedReward,
                   state::build_data::records::rewards::kRewardPerRecordCapacity>
            generated{};
        if (!state::build_data::find_generated_record_rewards(recordHash, generated, rewardCount)) {
            report_record_reward(message, "reward_table", recordIndex, 0, 0);
            return RecordRewardPreparation::failed;
        }
        if (rewardCount == 0) {
            return RecordRewardPreparation::absent;
        }
        for (std::size_t index = 0; index < rewardCount; ++index) {
            rewards[index] = {generated[index].itemDefinitionIndex, generated[index].quantity};
        }
    }

    auto* grant = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    if (grant == nullptr) {
        report_record_reward(
            message, "storage", recordIndex, rewards[0].itemDefinitionIndex, rewards[0].quantity);
        return RecordRewardPreparation::failed;
    }
    if (!state::prepare_record_reward_grant(std::span(rewards).first(rewardCount), claim, *grant)) {
        clear_mutation(outcome);
        report_record_reward(
            message, "state", recordIndex, rewards[0].itemDefinitionIndex, rewards[0].quantity);
        return RecordRewardPreparation::failed;
    }
    return RecordRewardPreparation::prepared;
}

/** Claims one exact reward-array row from the active Season of Arrivals pass. */
void claim_season_pass_reward(const middleware::web_service::Message& message,
                              Outcome& outcome) noexcept {
    namespace pass = state::progression::season_pass;
    namespace experience = state::progression::seasonal_experience;
    middleware::web_service::messages::opcode2400::Request request{};
    const pass::Reward* reward = nullptr;
    state::build_data::items::Definition item{};
    const std::uint16_t rank = experience::rank();
    const auto fail = [&](std::string_view reason) noexcept {
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws2400 stage=claim result=fail reason=%.*s transaction=%u progression=%u "
            "reward=%u rank=%u item_hash=0x%08X item_index=%u",
            static_cast<int>(reason.size()),
            reason.data(),
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned>(request.progressionIndex),
            static_cast<unsigned>(request.rewardIndex),
            static_cast<unsigned>(rank),
            reward == nullptr ? 0U : reward->itemHash,
            static_cast<unsigned>(item.definitionIndex));
        write_warning(line, count);
    };

    if (!middleware::web_service::messages::opcode2400::parse_request(message, request)) {
        return fail("payload_bits");
    }
    if (request.progressionIndex != pass::kProgressionDefinitionIndex) {
        return fail("progression");
    }
    reward = pass::find(request.rewardIndex);
    if (reward == nullptr) {
        return fail("reward_index");
    }
    if (reward->requiredRank > rank) {
        return fail("rank");
    }
    if (experience::reward_claimed(request.rewardIndex)) {
        return fail("already_claimed");
    }
    if (!state::build_data::find_item_definition_hash(reward->itemHash, item)) {
        return fail("item_hash");
    }
    auto* grant = emplace_mutation<state::PendingSeasonPassReward>(outcome);
    if (grant == nullptr) {
        return fail("storage");
    }
    if (const auto* package = pass::find_premium_class_package(reward->itemHash)) {
        auto& bundle = grant->grant.emplace<state::PendingDirectItemBundle>();
        if (!prepare_premium_class_package(*package, bundle)) {
            clear_mutation(outcome);
            return fail("package_grant");
        }
    } else if (reward->itemHash == pass::kDestinationResourceBundleHash) {
        if (!prepare_destination_resource_bundle(*grant)) {
            clear_mutation(outcome);
            return fail("resource_bundle");
        }
    } else if (reward->itemHash == pass::kLegendaryEngramHash
               || reward->itemHash == pass::kExoticEngramHash) {
        std::uint16_t decryptedItemIndex = 0;
        if (!choose_engram_reward(reward->itemHash, decryptedItemIndex)) {
            clear_mutation(outcome);
            return fail("engram_pool");
        }
        if (const char* reason = prepare_direct_reward(decryptedItemIndex, 1, *grant)) {
            clear_mutation(outcome);
            return fail(reason);
        }
    } else {
        if (const char* reason =
                prepare_direct_reward(item.definitionIndex, reward->quantity, *grant)) {
            clear_mutation(outcome);
            return fail(reason);
        }
        if (auto* armour = std::get_if<state::PendingItemAcquisition>(&grant->grant);
            armour != nullptr && !apply_season_armour_roll(request.rewardIndex, *armour)) {
            clear_mutation(outcome);
            return fail("armour_roll");
        }
    }
    grant->sourceDefinitionHash = reward->itemHash;
    grant->rewardIndex = request.rewardIndex;
    grant->prepared = true;
}

/** Decodes one opcode-1801 Triumphs claim and reports the record it names. */
void claim_record(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace records = state::build_data::records;
    middleware::web_service::messages::opcode1801::Request request{};
    if (!middleware::web_service::messages::opcode1801::parse_request(message, request)) {
        report_record_claim(message, "payload_bits", 0, records::kUnavailableFlagIndex, 0);
        return;
    }
    records::Definition definition{};
    if (!state::build_data::find_record_definition(request.recordIndex, definition)) {
        report_record_claim(
            message, "record_definition", request.recordIndex, records::kUnavailableFlagIndex, 0);
        return;
    }
    if (definition.completionFlagIndex == records::kUnavailableFlagIndex) {
        // Interval Triumphs intentionally carry no completion flag. Their repeated redemption
        // count is projected through the second reserved objective-value slot instead.
        if (state::record_claims::claim_interval(request.recordIndex,
                                                 definition.definitionHash)) {
            outcome.hasRecordClaim = true;
            return;
        }
        report_record_claim(message,
                            "interval_or_flag_unavailable",
                            request.recordIndex,
                            records::kUnavailableFlagIndex,
                            definition.scoreValue);
        return;
    }
    if (state::record_claims::claimed(definition.completionFlagIndex)) {
        report_record_claim(message,
                            "claim_rejected",
                            request.recordIndex,
                            definition.completionFlagIndex,
                            definition.scoreValue);
        return;
    }

    const state::record_claims::PendingClaim pendingClaim{definition.completionFlagIndex,
                                                          definition.scoreValue};
    const RecordRewardPreparation reward = prepare_record_reward(
        message, request.recordIndex, definition.definitionHash, pendingClaim, outcome);
    if (reward == RecordRewardPreparation::failed) {
        return;
    }
    if (reward == RecordRewardPreparation::prepared) {
        return;
    }
    if (!state::record_claims::claim(definition.completionFlagIndex, definition.scoreValue)) {
        report_record_claim(message,
                            "claim_rejected",
                            request.recordIndex,
                            definition.completionFlagIndex,
                            definition.scoreValue);
        return;
    }
    outcome.hasRecordClaim = true;
}

/** Equips an earned title record on the selected character. */
void equip_title(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace records = state::build_data::records;
    middleware::web_service::messages::opcode1821::Request request{};
    records::Definition definition{};
    std::uint64_t characterSoid = 0;
    bool changed = false;
    const char* reason = "payload_bits";
    if (middleware::web_service::messages::opcode1821::parse_request(message, request)) {
        if (request.recordIndex
            == middleware::web_service::messages::opcode1821::kUnequippedRecordIndex) {
            reason = "selected_character";
            if (state::set_selected_title(
                    state::kUnequippedTitleRecordIndex, characterSoid, changed)) {
                outcome.hasTitleEquip = true;
                return;
            }
        } else {
            reason = "record_definition";
            if (state::build_data::find_record_definition(request.recordIndex, definition)) {
                reason = "not_title";
                if (definition.hasTitle) {
                    reason = "not_claimed";
                    if (definition.completionFlagIndex != records::kUnavailableFlagIndex
                        && state::record_claims::claimed(definition.completionFlagIndex)) {
                        reason = "selected_character";
                        if (state::set_selected_title(
                                request.recordIndex, characterSoid, changed)) {
                            outcome.hasTitleEquip = true;
                            return;
                        }
                    }
                }
            }
        }
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=title_equip result=fail reason=%s opcode=%u transaction=%u record=%u "
                      "definition_hash=0x%08X completion_flag=%u character=0x%llX",
                      reason,
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      static_cast<unsigned>(request.recordIndex),
                      definition.definitionHash,
                      static_cast<unsigned>(definition.completionFlagIndex),
                      static_cast<unsigned long long>(characterSoid));
    write_warning(line, count);
}

} // namespace sunrise::server::web_service
