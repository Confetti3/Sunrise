#include "web_service_actions.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode1821.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode2400.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode504.h"
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

} // namespace

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
    } else {
        if (const char* reason =
                prepare_direct_reward(item.definitionIndex, reward->quantity, *grant)) {
            clear_mutation(outcome);
            return fail(reason);
        }
    }
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
        // The record carries no completion flag, or its slot has no row in the account bank.
        report_record_claim(message,
                            "no_completion_flag",
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
