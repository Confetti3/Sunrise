#include "web_service_actions.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../core/settings/rule_text.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode701/opcode701_codec.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/messages/opcode904/opcode904_codec.h"
#include "../../state/account/account_state.h"
#include "../../state/account/pursuit_hold.h"
#include "../../state/build_data/items/item_catalog.h"
#include "../../state/build_data/runtime.h"
#include "../../state/build_data/vendors/vendor_catalog.h"
#include "../../state/runtime/runtime.h"
#include "../../state/vendors/answered_interactions.h"
#include "internal.h"

namespace sunrise::server::web_service {

namespace {

/** Socket kind the shader model occupies, which is the only kind a shader swap may target. */
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;
/** Index stored when no definition resolves. The catalog is u16-indexed, so this cannot be one. */
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Repeatable bounties a character may hold from one vendor at once, as retail allows. */
constexpr std::uint32_t kRepeatableHoldLimit = 5;
/** Authored repeatable pool ceiling. The largest set in the manifest is Eva's Dawning, at 22. */
constexpr std::size_t kRepeatablePoolCapacity = 64;
/** Stacks one exchange row may credit. Shader recycling pays two: Glimmer and Legendary Shards. */
constexpr std::size_t kExchangePayoutCapacity = 4;
// Every credited stack is announced to the account's change ring, so a rule that named more
// payouts than the mutation can announce would pay out silently. Raising one raises the other.
static_assert(kExchangePayoutCapacity <= state::kProfileStackChangeCapacity);

/**
 * Storage every rule reader in this file parses from.
 *
 * The three readers run one after another on the request thread, each reading its file and
 * finishing with it before the next starts, so they share one buffer rather than holding one
 * each. A reader must not keep a cursor into it across a call to another reader.
 */
std::array<char, core::rule_text::kRuleTextCapacity> g_ruleText{};
/** One line carries the picked id and whether the selection moved. */
constexpr std::size_t kSelectLineCapacity = 96;

/** Reads the shared opcode-403/404 SOID descriptor through its codec. */
[[nodiscard]] bool parse_equipment_instance(const middleware::web_service::Message& message,
                                            std::uint64_t& instanceSoid) noexcept {
    middleware::web_service::messages::opcode403::Request request{};
    const bool parsed =
        middleware::web_service::messages::opcode403::parse_request(message, request);
    instanceSoid = request.instanceSoid;
    return parsed;
}

/** Records strict opcode-402 parsing, identity checks, and State preparation outcomes. */
void report_dismantle_preparation(const middleware::web_service::Message& message,
                                  std::string_view result,
                                  std::string_view reason,
                                  std::uint64_t instanceSoid,
                                  std::uint32_t definitionIndex,
                                  std::uint32_t definitionHash,
                                  std::uint32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws402 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "instance=0x%llX definition_index=%u definition_hash=0x%08X quantity=%u",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        static_cast<unsigned long long>(instanceSoid),
        definitionIndex,
        definitionHash,
        quantity);
    report_line(result == "ok" ? core::log::Level::debug : core::log::Level::warn, line, count);
}

/** Records strict opcode-1820 parsing, installed mapping, and State preparation outcomes. */
void report_acquisition_preparation(const middleware::web_service::Message& message,
                                    std::string_view result,
                                    std::string_view reason,
                                    std::uint32_t collectibleIndex,
                                    std::uint32_t itemDefinitionIndex,
                                    std::uint32_t definitionHash,
                                    std::uint64_t instanceSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1820 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "collectible_index=%u item_definition_index=%u definition_hash=0x%08X instance=0x%llX",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        collectibleIndex,
        itemDefinitionIndex,
        definitionHash,
        static_cast<unsigned long long>(instanceSoid));
    report_line(result == "ok" ? core::log::Level::debug : core::log::Level::warn, line, count);
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
        outcome.mutation = mutation;
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

/** Logs one exact correlated equipment response after its Queuez update is staged. */
void report_equip_response(const middleware::web_service::Message& message,
                           std::int32_t family4Version,
                           std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=equipment stage=response opcode=%u transaction=%u "
                                     "family_version=%d bytes=%zu hex=",
                                     static_cast<unsigned>(message.opcode),
                                     static_cast<unsigned>(message.transactionId),
                                     family4Version,
                                     response.size());
    report_response_line(line, prefix, response);
}

/** Logs the final item-creation status pair and the exact Family-4 revision it promises. */
void report_item_acquisition_response(const middleware::web_service::Message& message,
                                      std::int32_t family4Version,
                                      std::uint64_t acquiredInstanceSoid,
                                      std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(acquiredInstanceSoid),
        response.size());
    report_response_line(line, prefix, response);
}

/** Logs the final profile-stack status pair and the exact Family-4 account revision it promises. */
void report_profile_item_acquisition_response(const middleware::web_service::Message& message,
                                              std::int32_t family4Version,
                                              std::uint32_t definitionHash,
                                              std::int32_t quantity,
                                              std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=profile_acquire stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d definition_hash=0x%08X quantity=%d bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      definitionHash,
                      quantity,
                      response.size());
    report_response_line(line, prefix, response);
}

/** Logs the final dismantle status pair and the exact Family-4 revision it promises. */
void report_item_dismantle_response(const middleware::web_service::Message& message,
                                    std::int32_t family4Version,
                                    std::uint64_t dismantledInstanceSoid,
                                    std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=dismantle stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(dismantledInstanceSoid),
        response.size());
    report_response_line(line, prefix, response);
}

/** Logs the exact opcode-903 status pair and the item-instance revision it promises. */
void report_socket_plug_response(const middleware::web_service::Message& message,
                                 std::int32_t family4Version,
                                 std::uint64_t targetInstanceSoid,
                                 std::uint8_t socketLane,
                                 std::uint16_t plugDefinitionIndex,
                                 std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX lane=%u plug_definition=%u bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(targetInstanceSoid),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        response.size());
    report_response_line(line, prefix, response);
}

/** Logs the exact opcode-801 status pair and subclass item revision it promises. */
void report_subclass_selection_response(const middleware::web_service::Message& message,
                                        std::int32_t family4Version,
                                        const state::PendingSubclassSelection& mutation,
                                        std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=subclass_select stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d instance=0x%llX entry=%u bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      static_cast<unsigned long long>(mutation.subclassInstanceSoid),
                      static_cast<unsigned>(mutation.requestedEntry),
                      response.size());
    report_response_line(line, prefix, response);
}

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way.
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
    outcome.selectedCharacterSoid = picked.characterSoid;

    std::array<char, kSelectLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=ws504 stage=select result=ok soid=0x%llX changed=%u",
                                      static_cast<unsigned long long>(picked.characterSoid),
                                      static_cast<unsigned>(changed));
    report_line(core::log::Level::debug, line, written);
}

/** Prepares one opcode-403/404 equipment mutation without publishing State early. */
void mutate_equipment(const middleware::web_service::Message& message,
                      bool unequip,
                      Outcome& outcome) noexcept {
    std::uint64_t requestedInstanceSoid = 0;
    if (!parse_equipment_instance(message, requestedInstanceSoid)) {
        std::array<char, 112> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=equipment stage=parse result=fail opcode=%u "
                                        "payload_bytes=%zu",
                                        static_cast<unsigned>(message.opcode),
                                        message.payload.size());
        report_line(core::log::Level::warn, line, count);
        return;
    }

    state::PendingEquipmentSwap mutation;
    const bool prepared = unequip
                              ? state::prepare_equipment_unequip(requestedInstanceSoid, mutation)
                              : state::prepare_equipment_swap(requestedInstanceSoid, mutation);
    if (!prepared) {
        std::array<char, 144> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=equipment stage=prepare result=fail opcode=%u action=%s requested=0x%llX",
            static_cast<unsigned>(message.opcode),
            unequip ? "unequip" : "equip",
            static_cast<unsigned long long>(requestedInstanceSoid));
        report_line(core::log::Level::warn, line, count);
        return;
    }
    outcome.mutation = mutation;

    std::array<char, 224> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=equipment stage=prepare result=ok opcode=%u action=%s character=0x%llX "
                      "previous=0x%llX requested=0x%llX native_slot=%u moved_items=%zu",
                      static_cast<unsigned>(message.opcode),
                      unequip ? "unequip" : "equip",
                      static_cast<unsigned long long>(mutation.characterSoid),
                      static_cast<unsigned long long>(mutation.previousInstanceSoid),
                      static_cast<unsigned long long>(mutation.requestedInstanceSoid),
                      static_cast<unsigned>(mutation.nativeEquipmentSlot),
                      mutation.movedItemCount);
    report_line(core::log::Level::debug, line, count);
}

/** Parses and prepares one exact selected-character opcode-801 subclass node selection. */
void mutate_subclass_selection(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode801::Request request{};
    if (!middleware::web_service::messages::opcode801::parse_request(message, request)) {
        std::array<char, 128> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=ws801 stage=parse result=fail transaction=%u payload_bytes=%zu",
                          static_cast<unsigned>(message.transactionId),
                          message.payload.size());
        report_line(core::log::Level::warn, line, count);
        return;
    }

    state::PendingSubclassSelection mutation{};
    if (!state::prepare_subclass_selection(
            request.subclassInstanceSoid, request.socketEntry, mutation)) {
        std::array<char, 160> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws801 stage=prepare result=fail transaction=%u instance=0x%llX entry=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.subclassInstanceSoid),
            static_cast<unsigned>(request.socketEntry));
        report_line(core::log::Level::warn, line, count);
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws801 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "entry=%u socket_list=%u",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.subclassInstanceSoid),
        static_cast<unsigned>(mutation.requestedEntry),
        static_cast<unsigned>(mutation.socketEntryListIndex));
    report_line(core::log::Level::info, line, count);
}

/** Parses and prepares one exact selected-character opcode-903 socket selection. */
void mutate_socket_plug(const middleware::web_service::Message& message,
                        Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode903::Request request{};
    if (!middleware::web_service::messages::opcode903::parse_request(message, request)
        || !request.hasInstance || request.instanceSoid == 0 || request.hasTargetDefinition
        || !request.hasPlugDefinition
        || request.socketIndex >= state::account::inventory::kPlugCapacity) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=parse result=fail transaction=%u payload_bytes=%zu has_instance=%u "
            "instance=0x%llX has_target_definition=%u socket=%u has_plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned>(request.hasInstance),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.hasTargetDefinition),
            request.socketIndex,
            static_cast<unsigned>(request.hasPlugDefinition));
        report_line(core::log::Level::warn, line, count);
        return;
    }

    state::PendingSocketPlug mutation{};
    if (!state::prepare_socket_plug(request.instanceSoid,
                                    static_cast<std::uint8_t>(request.socketIndex),
                                    request.plugDefinitionIndex,
                                    mutation)) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=prepare result=fail transaction=%u instance=0x%llX lane=%u "
            "plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.instanceSoid),
            request.socketIndex,
            static_cast<unsigned>(request.plugDefinitionIndex));
        report_line(core::log::Level::warn, line, count);
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 240> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws903 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "target_definition=%u target_bucket=%u lane=%u plug_definition=%u plug_bucket=%u "
        "equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(mutation.targetEquipped),
        mutation.itemIndex);
    report_line(core::log::Level::debug, line, count);
}

/** Parses and prepares one character-location opcode-1901 socket selection. */
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept {
    namespace opcode1901 = middleware::web_service::messages::opcode1901;
    opcode1901::Request request{};
    const bool parsed = opcode1901::parse_request(message, request);
    // A request prepares at most one State mutation, so a run naming several sockets is declined
    // rather than treated as malformed. The reply carries that refusal.
    const opcode1901::Replacement& replacement = request.replacements.front();
    if (!parsed || request.replacementCount != 1
        || replacement.modelSocketKind != kEquippedShaderModelSocketKind
        || replacement.auxiliary != 0
        || replacement.socketIndex >= state::account::inventory::kPlugCapacity
        || request.instanceIdentityToken == 0) {
        std::array<char, 256> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=parse result=fail transaction=%u payload_bytes=%zu replacements=%zu "
            "plug_definition=%u canonical_kind=%u model_kind=%u socket=%u auxiliary=0x%llX "
            "equipment_selector=%llu",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            request.replacementCount,
            static_cast<unsigned>(replacement.plugDefinitionIndex),
            static_cast<unsigned>(replacement.canonicalSocketKind),
            static_cast<unsigned>(replacement.modelSocketKind),
            replacement.socketIndex,
            static_cast<unsigned long long>(replacement.auxiliary),
            static_cast<unsigned long long>(request.equipmentSelector));
        report_line(core::log::Level::warn, line, count);
        return;
    }

    const std::uint64_t identityToken = request.instanceIdentityToken;
    state::PendingSocketPlug mutation{};
    if (!state::prepare_character_selector_socket_plug(
            request.instanceIdentityToken,
            static_cast<std::uint8_t>(replacement.socketIndex),
            replacement.plugDefinitionIndex,
            mutation)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=prepare result=fail transaction=%u equipment_selector=%llu "
            "identity_token=%llu lane=%u plug_definition=%u canonical_kind=%u model_kind=%u "
            "auxiliary=0x%llX",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.equipmentSelector),
            static_cast<unsigned long long>(identityToken),
            replacement.socketIndex,
            static_cast<unsigned>(replacement.plugDefinitionIndex),
            static_cast<unsigned>(replacement.canonicalSocketKind),
            static_cast<unsigned>(replacement.modelSocketKind),
            static_cast<unsigned long long>(replacement.auxiliary));
        report_line(core::log::Level::warn, line, count);
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 288> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1901 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "equipment_selector=%llu identity_token=%llu target_definition=%u target_bucket=%u "
        "lane=%u plug_definition=%u plug_bucket=%u canonical_kind=%u model_kind=%u "
        "auxiliary=0x%llX",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned long long>(request.equipmentSelector),
        static_cast<unsigned long long>(identityToken),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(replacement.canonicalSocketKind),
        static_cast<unsigned>(replacement.modelSocketKind),
        static_cast<unsigned long long>(replacement.auxiliary));
    report_line(core::log::Level::debug, line, count);
}

/** Parses and prepares one complete accumulated item-state value from opcode 406. */
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode406::Request request{};
    if (!middleware::web_service::messages::opcode406::parse_request(message, request)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws406 stage=parse result=fail transaction=%u payload_bytes=%zu instance=0x%llX "
            "definition=%u flags=0x%X",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.definitionIndex),
            request.flags);
        report_line(core::log::Level::warn, line, count);
        return;
    }

    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint32_t flags = request.flags;
    state::PendingItemState mutation{};
    if (!state::prepare_item_state(instanceSoid, request.definitionIndex, flags, mutation)) {
        return;
    }
    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws406 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "definition=%u flags_before=0x%X flags_after=0x%X equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        mutation.beforeFlags,
        mutation.afterFlags,
        mutation.targetEquipped ? 1U : 0U,
        mutation.itemIndex);
    report_line(core::log::Level::debug, line, count);
}

/** Prepares the exact fixed-width opcode-402 Character-inventory removal request. */
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode402::Request request{};
    if (!middleware::web_service::messages::opcode402::parse_request(message, request)) {
        report_dismantle_preparation(
            message, "fail", "payload_bits", request.instanceSoid, request.definitionIndex, 0, 0);
        return;
    }
    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint16_t definitionIndex = request.definitionIndex;
    // The codec owns the value; this alias keeps the dismantle checks below readable.
    constexpr std::uint32_t kSingleQuantity =
        middleware::web_service::messages::opcode402::kSingleQuantity;

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)) {
        report_dismantle_preparation(
            message, "fail", "definition", instanceSoid, definitionIndex, 0, kSingleQuantity);
        return;
    }
    state::PendingItemDismantle mutation{};
    if (!state::prepare_item_dismantle(instanceSoid, mutation)) {
        report_dismantle_preparation(message,
                                     "fail",
                                     "state",
                                     instanceSoid,
                                     definitionIndex,
                                     definition.definitionHash,
                                     kSingleQuantity);
        return;
    }
    if (mutation.dismantledItem.definitionHash != definition.definitionHash
        || mutation.dismantledItem.quantity != static_cast<std::int32_t>(kSingleQuantity)) {
        report_dismantle_preparation(message,
                                     "fail",
                                     "identity",
                                     instanceSoid,
                                     definitionIndex,
                                     definition.definitionHash,
                                     kSingleQuantity);
        return;
    }
    outcome.mutation = mutation;
    report_dismantle_preparation(message,
                                 "ok",
                                 "ready",
                                 instanceSoid,
                                 definitionIndex,
                                 definition.definitionHash,
                                 kSingleQuantity);
}

/**
 * Writes one purchase line.
 *
 * The opcode is carried rather than hard-coded: 901 and 904 share this line, and a quest acquire
 * reporting itself as `ws901` sends anyone reading the log to the wrong decoder.
 *
 * @param opcode Request opcode the line belongs to, 901 or 904.
 * @param result `ok` or `fail`.
 * @param reason Step that decided it.
 * @param vendorIndex Vendor row the request named.
 * @param saleIndex Sale row the request named.
 * @param itemDefinitionIndex Item resolved, when the row resolved.
 */
void report_purchase(std::uint16_t opcode,
                     const char* result,
                     const char* reason,
                     std::int32_t vendorIndex,
                     std::int32_t saleIndex,
                     std::uint16_t itemDefinitionIndex) noexcept {
    core::log::writef(core::log::Channel::server,
                      std::strcmp(result, "ok") == 0 ? core::log::Level::info
                                                     : core::log::Level::warn,
                      "ev=ws%u stage=purchase result=%s reason=%s vendor=%d sale=%d item=%u",
                      static_cast<unsigned>(opcode),
                      result,
                      reason,
                      static_cast<int>(vendorIndex),
                      static_cast<int>(saleIndex),
                      static_cast<unsigned>(itemDefinitionIndex));
}

/**
 * Resolves the vendor a request names to its index row and held definition.
 *
 * Every vendor behaviour starts here, and five of them spelled it out by hand. A negative index is
 * the client's own absent marker and never a row.
 *
 * @param vendorIndex Vendor row the request named.
 * @param entry Receives the index row.
 * @param definition Receives the held definition.
 * @return True when the row exists and its definition is published.
 */
[[nodiscard]] bool find_vendor(std::int32_t vendorIndex,
                               state::build_data::vendors::IndexEntry& entry,
                               state::build_data::vendors::Definition& definition) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    entry = {};
    definition = {};
    return vendorIndex >= 0 && vendorIndex <= (std::numeric_limits<std::uint16_t>::max)()
           && vendor_domain::find_index(static_cast<std::uint16_t>(vendorIndex), entry)
           && vendor_domain::find(entry.definitionHash, definition);
}

/** What a substitution rule said about one sale row's item. */
enum class Substitution : std::uint8_t {
    /** No rule names this item; the row grants what it names. */
    none,
    /** A rule names it and its replacement resolved; the row grants the replacement. */
    replaced,
    /** A rule names it but its replacement is not in this build; the row must grant nothing. */
    broken,
};

/**
 * Answers what a placeholder sale row is really selling.
 *
 * Several rows name a DestinyItemType 20 Dummy - a UI placeholder for something the row does not
 * name, as Amanda Holliday's Legacy Content rows stand for a campaign's first quest step. Granting
 * the placeholder puts an item in the Quests tab the client will not draw, and the row never
 * settles. `vendor_item_substitute.txt` maps sold hash to granted hash, keyed by item so one rule
 * covers every seller. A rule whose replacement is absent from this build answers `broken` rather
 * than `none`: the rule proves the row's item is a placeholder, and granting it would be the exact
 * wrong grant this file exists to prevent.
 *
 * @param itemDefinitionIndex Item the row resolved to.
 * @param substituteIndex Receives what should be granted in its place.
 * @return What the rule file said about this item.
 */
[[nodiscard]] Substitution substitute_for_item(std::uint16_t itemDefinitionIndex,
                                               std::uint16_t& substituteIndex) noexcept {
    substituteIndex = kUnavailableDefinitionIndex;
    state::build_data::items::Definition sold{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, sold)) {
        return Substitution::none;
    }
    if (!core::path::read_artifact_text(L"vendor_item_substitute.txt", g_ruleText)) {
        return Substitution::none;
    }
    core::rule_text::Cursor rules{g_ruleText.data()};
    while (rules.seek_field()) {
        const std::uint32_t soldHash = rules.read_hex();
        const std::uint32_t grantHash = rules.read_hex();
        if (soldHash != sold.definitionHash) {
            continue;
        }
        state::build_data::items::Definition replacement{};
        const bool resolved =
            state::build_data::find_item_definition_hash(grantHash, replacement);
        if (resolved) {
            substituteIndex = replacement.definitionIndex;
        }
        if (resolved) {
            core::log::writef(core::log::Channel::server,
                              core::log::Level::info,
                              "ev=vendor stage=substitute sold=0x%08X granted=0x%08X item=%u",
                              sold.definitionHash,
                              replacement.definitionHash,
                              static_cast<unsigned>(replacement.definitionIndex));
            return Substitution::replaced;
        }
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=vendor stage=substitute result=fail reason=missing sold=0x%08X "
                          "named=0x%08X",
                          sold.definitionHash,
                          grantHash);
        return Substitution::broken;
    }
    return Substitution::none;
}

/**
 * Rolls one random unheld repeatable bounty, for a row that offers "Additional Bounties".
 *
 * The row sells a Dummy placeholder; what it owes is a REPEATABLE bounty, a distinct kind a
 * character may hold five of. The pool is authored by hash in `vendor_bounty_roll.txt`, because a
 * repeatable is not a sale row - no vendor in the manifest lists one - so nothing on the vendor can
 * be discovered or picked from. Rules are keyed by vendor definition hash and trigger category,
 * since one vendor can own several such rows (Eva Levante has one per event), and lines sharing a
 * key accumulate. A hash this build does not carry is skipped, so a pool authored from a newer
 * manifest degrades to what exists rather than failing whole.
 *
 * @param vendorIndex Vendor the purchase names.
 * @param categoryIndex Category of the purchased row, from sale row +100.
 * @param rolledItemIndex Receives the bounty to grant.
 * @return True when this row is a bounty roll and its own item must NOT be granted.
 */
[[nodiscard]] bool roll_vendor_bounty(std::int32_t vendorIndex,
                                      std::int32_t categoryIndex,
                                      std::uint16_t& rolledItemIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    rolledItemIndex = kUnavailableDefinitionIndex;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (categoryIndex < 0 || !find_vendor(vendorIndex, entry, definition)) {
        return false;
    }
    if (!core::path::read_artifact_text(L"vendor_bounty_roll.txt", g_ruleText)) {
        return false;
    }
    // Every hash authored for this exact key. Lines carrying the same key accumulate, so the pool
    // is gathered from the whole file rather than from the first line that matches.
    std::array<std::uint32_t, kRepeatablePoolCapacity> pool{};
    std::size_t poolCount = 0;
    core::rule_text::Cursor rules{g_ruleText.data()};
    while (rules.seek_field()) {
        const std::uint32_t ruleHash = rules.read_hex();
        const std::int32_t ruleCategory = rules.read_decimal();
        const bool wanted = ruleHash == entry.definitionHash && ruleCategory == categoryIndex;
        // The rest of the line is item hashes. A newline is not a rule field, so this stops at the
        // end of the line without needing to look for one.
        while (rules.at_field()) {
            const std::uint32_t itemHash = rules.read_hex();
            if (wanted && poolCount < pool.size()) {
                pool[poolCount++] = itemHash;
            }
        }
    }
    if (poolCount == 0) {
        return false;
    }
    // Reservoir pick over what this build actually carries and the character does not already hold,
    // so the pool is walked once and no count is needed up front.
    std::uint32_t resolved = 0;
    std::uint32_t held = 0;
    std::uint32_t candidates = 0;
    std::uint64_t seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    // One account view for the whole pool. Reading it copies the whole account, and the pool is
    // walked candidate by candidate, so taking it per candidate would copy it dozens of times to
    // answer dozens of questions about the same unchanging view.
    const state::AccountState account = state::account_snapshot();
    for (std::size_t at = 0; at < poolCount; ++at) {
        state::build_data::items::Definition item{};
        if (!state::build_data::items::find_hash(pool[at], item)) {
            continue;
        }
        ++resolved;
        if (state::account::holds_pursuit(account, item.definitionIndex)) {
            ++held;
            continue;
        }
        ++candidates;
        seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
        if ((seed >> 33) % candidates == 0) {
            rolledItemIndex = item.definitionIndex;
        }
    }
    // Retail lets a character keep five of a vendor's repeatables at once. Refusing here rather
    // than at the grant keeps the roll from consuming a pick it would only have to throw away.
    if (held >= kRepeatableHoldLimit) {
        rolledItemIndex = kUnavailableDefinitionIndex;
    }
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=bounty_roll stage=pick vendor=%d hash=0x%08X category=%d authored=%u "
                      "resolved=%u held=%u pool=%u item=%d",
                      vendorIndex,
                      entry.definitionHash,
                      categoryIndex,
                      static_cast<unsigned>(poolCount),
                      resolved,
                      held,
                      candidates,
                      rolledItemIndex == kUnavailableDefinitionIndex
                          ? -1
                          : static_cast<int>(rolledItemIndex));
    return true;
}

/**
 * Runs a vendor's recycle row: charges the stack it names and credits what it pays out.
 *
 * The Drifter's four Synth Recycling rows take five synths each; Master Rahool's Recycle Shaders
 * category has one row per shader, 277 of them. The cost is authored in `vendor_exchange.txt`
 * rather than read off the row, because the sale row's cost-bearing fields are still role-open on
 * this build; the manifest's row order is this build's (304 rows checked against Lord Shaxx). A
 * rule is `<vendor> <row> <costItem> <costQuantity>` then `<payoutItem> <payoutQuantity>` pairs.
 *
 * @param vendorIndex Vendor the purchase names.
 * @param rowIndex Sale row the purchase names.
 * @param mutation Receives the prepared profile-stack change.
 * @return True when this row was an exchange and its own item must NOT be granted.
 */
[[nodiscard]] bool
exchange_vendor_row(std::int32_t vendorIndex,
                    std::int32_t rowIndex,
                    state::PendingProfileItemAcquisition& mutation) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (rowIndex < 0 || !find_vendor(vendorIndex, entry, definition)) {
        return false;
    }
    if (!core::path::read_artifact_text(L"vendor_exchange.txt", g_ruleText)) {
        return false;
    }
    std::uint32_t costHash = 0;
    std::int32_t costQuantity = 0;
    std::array<state::ProfileExchangePayout, kExchangePayoutCapacity> payouts{};
    std::size_t payoutCount = 0;
    bool matched = false;
    bool overflowed = false;
    core::rule_text::Cursor rules{g_ruleText.data()};
    while (!matched && rules.seek_field()) {
        const std::uint32_t ruleVendor = rules.read_hex();
        const std::int32_t ruleRow = rules.read_decimal();
        const std::uint32_t ruleCost = rules.read_hex();
        const std::int32_t ruleCostQuantity = rules.read_decimal();
        // The rest of the line is payout pairs, and every one of them is consumed even past what
        // can be held. Stopping mid-line would leave the fields that did not fit to be read as the
        // start of the next rule, turning one over-long rule into a second, invented one.
        std::array<state::ProfileExchangePayout, kExchangePayoutCapacity> rulePayouts{};
        std::size_t rulePayoutCount = 0;
        bool ruleOverflowed = false;
        while (rules.at_field()) {
            const std::uint32_t payoutHash = rules.read_hex();
            const std::int32_t payoutQuantity = rules.read_decimal();
            if (rulePayoutCount < rulePayouts.size()) {
                rulePayouts[rulePayoutCount++] = {payoutHash, payoutQuantity};
            } else {
                ruleOverflowed = true;
            }
        }
        matched = ruleVendor == entry.definitionHash && ruleRow == rowIndex;
        if (matched) {
            overflowed = ruleOverflowed;
            costHash = ruleCost;
            costQuantity = ruleCostQuantity;
            payouts = rulePayouts;
            payoutCount = rulePayoutCount;
        }
    }
    if (!matched) {
        return false;
    }
    // A matched rule owns the row whatever else it got wrong, because the rule proves the row's
    // own item is a placeholder and falling through would grant it. A rule naming more payouts
    // than the change ring can announce, or none at all, is refused whole rather than paid in
    // part - and the refusal is logged, because a rule that silently does nothing reads exactly
    // like a rule that was never written.
    if (overflowed || payoutCount == 0) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=vendor_exchange stage=apply result=fail reason=%s vendor=%d "
                          "hash=0x%08X row=%d payouts=%zu limit=%zu",
                          overflowed ? "payout_overflow" : "payout_missing",
                          vendorIndex,
                          entry.definitionHash,
                          rowIndex,
                          payoutCount,
                          kExchangePayoutCapacity);
        return true;
    }
    const bool applied = state::prepare_vendor_exchange(
        costHash, costQuantity,
        std::span<const state::ProfileExchangePayout>{payouts.data(), payoutCount}, mutation);
    core::log::writef(core::log::Channel::server,
                      applied ? core::log::Level::info : core::log::Level::warn,
                      "ev=vendor_exchange stage=apply result=%s vendor=%d hash=0x%08X row=%d "
                      "cost=0x%08X quantity=%d payouts=%zu",
                      applied ? "ok" : "fail",
                      vendorIndex,
                      entry.definitionHash,
                      rowIndex,
                      costHash,
                      costQuantity,
                      payoutCount);
    // Even a refused exchange owns the row. Falling through would grant the Dummy placeholder,
    // which is the failure this whole path exists to avoid.
    return true;
}

/** How one grant ended, so a caller can tell a settled row from a row still owed its item. */
enum class GrantResult : std::uint8_t {
    /** The item is prepared for the inventory; the row's offer is answered. */
    granted,
    /** The character already holds this pursuit, so the offer was answered some time ago. */
    alreadyHeld,
    /** Nothing was granted and nothing was held; the offer still stands. */
    refused,
};

/**
 * Grants one item, given the collectible that owns it and its definition index.
 *
 * Split out of `acquire_item` so a vendor purchase reaches the same grant instead of growing a
 * second acquisition path. The acquisition state is keyed by collectible, so a caller has to arrive
 * with one; `find_collectible_for_item` is how a purchase gets there.
 *
 * @param message Request being answered, for the log line.
 * @param collectibleIndex Collectible that owns the item.
 * @param itemDefinitionIndex Item to grant.
 * @param outcome Receives the prepared mutation on success.
 * @return How the grant ended, which is what decides whether the row's offer was answered.
 */
GrantResult grant_item_definition(const middleware::web_service::Message& message,
                                  std::uint16_t collectibleIndex,
                                  std::uint16_t itemDefinitionIndex,
                                  Outcome& outcome) noexcept {
    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        report_acquisition_preparation(
            message, "fail", "item_definition", collectibleIndex, itemDefinitionIndex, 0, 0);
        return GrantResult::refused;
    }
    // The same rule the client's native vendor-row gate applies locally, so a row that is still
    // offered can never be one this grant would refuse.
    if (state::account::holds_pursuit(itemDefinitionIndex)) {
        report_acquisition_preparation(message,
                                       "fail",
                                       "already_held",
                                       collectibleIndex,
                                       itemDefinitionIndex,
                                       definition.definitionHash,
                                       0);
        return GrantResult::alreadyHeld;
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        report_acquisition_preparation(message,
                                       "fail",
                                       "item_detail_or_bucket",
                                       collectibleIndex,
                                       itemDefinitionIndex,
                                       definition.definitionHash,
                                       0);
        return GrantResult::refused;
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            report_acquisition_preparation(message,
                                           "fail",
                                           "profile_item_instanced",
                                           collectibleIndex,
                                           itemDefinitionIndex,
                                           definition.definitionHash,
                                           0);
            return GrantResult::refused;
        }
        state::PendingProfileItemAcquisition mutation{};
        if (!state::prepare_profile_item_acquisition(
                collectibleIndex, definition.definitionHash, mutation)) {
            report_acquisition_preparation(message,
                                           "fail",
                                           "profile_state",
                                           collectibleIndex,
                                           itemDefinitionIndex,
                                           definition.definitionHash,
                                           0);
            return GrantResult::refused;
        }
        outcome.mutation = mutation;
        report_acquisition_preparation(message,
                                       "ok",
                                       "profile_ready",
                                       collectibleIndex,
                                       itemDefinitionIndex,
                                       definition.definitionHash,
                                       0);
        return GrantResult::granted;
    }
    if (bucket.arraySelector != bucket_domain::ArraySelector::character) {
        report_acquisition_preparation(message,
                                       "fail",
                                       "unsupported_inventory_array",
                                       collectibleIndex,
                                       itemDefinitionIndex,
                                       definition.definitionHash,
                                       0);
        return GrantResult::refused;
    }

    state::PendingItemAcquisition mutation{};
    if (!state::prepare_item_acquisition(collectibleIndex, definition.definitionHash, mutation)) {
        report_acquisition_preparation(message,
                                       "fail",
                                       "state",
                                       collectibleIndex,
                                       itemDefinitionIndex,
                                       definition.definitionHash,
                                       0);
        return GrantResult::refused;
    }
    outcome.mutation = mutation;
    report_acquisition_preparation(message,
                                   "ok",
                                   "ready",
                                   collectibleIndex,
                                   itemDefinitionIndex,
                                   definition.definitionHash,
                                   mutation.acquiredInstanceSoid);
    return GrantResult::granted;
}

/**
 * Finds the collectible that owns one item definition.
 *
 * A sale row names an item, never a collectible, while the acquisition state is keyed by
 * collectible. Bounties, tokens and quest steps have none at all; those are granted by hash under
 * `kNoCollectibleIndex`, which is why the caller's sentinel is left in place when nothing matches.
 *
 * @param itemDefinitionIndex Item to look up.
 * @param collectibleIndex Receives the owning collectible row; untouched when none does.
 * @return True when a collectible names this item.
 */
[[nodiscard]] bool find_collectible_for_item(std::uint16_t itemDefinitionIndex,
                                             std::uint16_t& collectibleIndex) noexcept {
    return state::build_data::collectibles::find_granting(itemDefinitionIndex, collectibleIndex);
}

/** Prepares the exact three-byte opcode-1820 Collections item request. */
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode1820::Request request{};
    if (!middleware::web_service::messages::opcode1820::parse_request(message, request)) {
        report_acquisition_preparation(message,
                                       "fail",
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
        report_acquisition_preparation(message,
                                       "fail",
                                       "collectible_definition",
                                       collectibleIndex,
                                       kUnavailableDefinitionIndex,
                                       0,
                                       0);
        return;
    }
    (void)grant_item_definition(message, collectibleIndex, itemDefinitionIndex, outcome);
}

/**
 * Resolves one vendor row to the item it sells.
 *
 * Shared by the purchase (901) and the quest acquire (904), which name a row the same way, so the
 * two cannot drift apart.
 *
 * @param vendorIndex Vendor table row.
 * @param rowIndex Sale row within that vendor.
 * @param itemDefinitionIndex Receives the item the row sells.
 * @param reason Receives the step that failed, when one does.
 * @return True when the row resolved.
 */
[[nodiscard]] bool resolve_vendor_row(std::int32_t vendorIndex,
                                      std::int32_t rowIndex,
                                      std::uint16_t& itemDefinitionIndex,
                                      std::int32_t& categoryIndex,
                                      const char*& reason) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    if (vendorIndex < 0 || rowIndex < 0) {
        reason = "negative_index";
        return false;
    }
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (!find_vendor(vendorIndex, entry, definition)) {
        reason = "vendor";
        return false;
    }
    vendor_domain::SaleRow row{};
    if (!vendor_domain::sale_row(definition, static_cast<std::size_t>(rowIndex), row)) {
        reason = "sale_row";
        return false;
    }
    itemDefinitionIndex = row.itemIndex;
    categoryIndex = row.categoryIndex;
    return true;
}

/** Pursuit rows written out when a vendor is asked what it actually sells. */
constexpr std::size_t kPursuitListCap = 64;

/**
 * Lists the sale rows of one vendor whose item is a pursuit, when a rowless tile fails to resolve.
 *
 * It says what this vendor does offer that would land in the Quests tab, which is the difference
 * between "this click is broken" and "this click was never a quest". Items rather than rows, because
 * one placeholder repeats across dozens of rows. The classification is the shared pursuit rule.
 *
 * @param vendorIndex Vendor to list.
 */
void report_pursuit_rows(std::int32_t vendorIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    namespace detail_domain = state::build_data::items::details;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (!find_vendor(vendorIndex, entry, definition)) {
        return;
    }
    const std::size_t count = definition.saleCount;
    // One item repeats across dozens of rows - Amanda declares 38 consecutive rows of a single
    // placeholder - so listing rows rather than items buries everything interesting under filler.
    static std::array<std::uint16_t, kPursuitListCap> seen{};
    std::size_t listed = 0;
    std::size_t pursuits = 0;
    for (std::size_t row = 0; row < count; ++row) {
        vendor_domain::SaleRow sale{};
        if (!vendor_domain::sale_row(definition, row, sale)) {
            break;
        }
        const std::uint16_t itemIndex = sale.itemIndex;
        detail_domain::Definition detail{};
        if (!state::build_data::find_configured_item_detail(itemIndex, detail)
            || detail.equipmentSlot.has_value() || detail.maxStackSize > 1) {
            continue;
        }
        ++pursuits;
        bool duplicate = false;
        for (std::size_t index = 0; index < listed; ++index) {
            duplicate = duplicate || seen[index] == itemIndex;
        }
        if (duplicate || listed >= kPursuitListCap) {
            continue;
        }
        seen[listed] = itemIndex;
        ++listed;
        // One line per distinct row, so this is the detail behind the summary rather than
        // something worth putting in front of everything else that reports at info.
        core::log::writef(core::log::Channel::server,
                          core::log::Level::debug,
                          "ev=vendor stage=pursuit vendor=%d sale=%zu item=%u hash=0x%08X "
                          "bucket=%u",
                          static_cast<int>(vendorIndex),
                          row,
                          static_cast<unsigned>(itemIndex),
                          detail.definitionHash,
                          static_cast<unsigned>(detail.bucketId));
    }
    core::log::writef(core::log::Channel::server,
                      core::log::Level::info,
                      "ev=vendor stage=pursuits vendor=%d sale_rows=%zu pursuits=%zu "
                      "distinct_listed=%zu",
                      static_cast<int>(vendorIndex),
                      count,
                      pursuits,
                      listed);
}

/** An installed row names its item by definition hash at this offset. */
constexpr std::size_t kInstalledRowHashOffset = 0;
/** FNV-1's basis, which this engine also uses as its absent-hash sentinel. */
constexpr std::uint32_t kAbsentNameHash = 0x811C9DC5U;

/**
 * Resolves the item behind a 904 that names no sale row.
 *
 * Amanda Holliday's Legacy Content tiles send `slot=1, row=-1`, so the slot is all that identifies
 * them - and it indexes the installed array: the Red War tile's vendor declares 220 sale rows but
 * 22 installed rows, and its slot is 1. That installed row carries the item's definition hash at
 * `+0`, where a sale row names its item by index. The resolution is logged either way, because a
 * wrong item that commits cleanly is harder to spot than a refusal.
 *
 * @param vendorIndex Vendor the request named.
 * @param slotIndex The 16-bit slot field, which is all the request carries.
 * @param itemDefinitionIndex Receives the item, or the unavailable sentinel.
 * @return True when the row's hash resolved to an installed item definition.
 */
[[nodiscard]] bool resolve_rowless_quest(std::int32_t vendorIndex,
                                         std::int32_t slotIndex,
                                         std::uint16_t& itemDefinitionIndex) noexcept {
    namespace vendor_domain = state::build_data::vendors;
    itemDefinitionIndex = kUnavailableDefinitionIndex;
    vendor_domain::IndexEntry entry{};
    vendor_domain::Definition definition{};
    if (slotIndex < 0 || !find_vendor(vendorIndex, entry, definition)) {
        return false;
    }
    vendor_domain::InstalledRow installed{};
    if (!vendor_domain::installed_row(definition, static_cast<std::size_t>(slotIndex), installed)) {
        return false;
    }
    const auto& raw = installed.raw;
    std::uint32_t definitionHash = 0;
    std::memcpy(&definitionHash, raw.data() + kInstalledRowHashOffset, sizeof definitionHash);

    state::build_data::items::Definition item{};
    const bool resolved = definitionHash != kAbsentNameHash
                          && state::build_data::find_item_definition_hash(definitionHash, item);
    if (resolved) {
        itemDefinitionIndex = item.definitionIndex;
    }
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=ws904 stage=rowless vendor=%d slot=%d installed=%u sale=%u "
                                "third=%u hash=0x%08X item=%u resolved=%u hex=",
                                static_cast<int>(vendorIndex),
                                static_cast<int>(slotIndex),
                                static_cast<unsigned>(definition.installedCount),
                                static_cast<unsigned>(definition.saleCount),
                                static_cast<unsigned>(definition.thirdCount),
                                definitionHash,
                                static_cast<unsigned>(itemDefinitionIndex),
                                resolved ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        std::size_t length = static_cast<std::size_t>(written);
        const auto* const bytes = reinterpret_cast<const std::byte*>(raw.data());
        (void)core::log::append_hex(line, length, {bytes, raw.size()});
        if (length != 0) {
            core::log::write(core::log::Channel::server,
                             resolved ? core::log::Level::info : core::log::Level::warn,
                             {line.data(), length});
        }
    }
    return resolved;
}

/** What one resolved vendor row turned out to be, once it was settled. */
enum class RowOutcome : std::uint8_t {
    /** The row rolled a bounty from an authored pool. */
    bountyRoll,
    /** The row charged one stack and credited others. */
    exchange,
    /** The row's item is prepared for the inventory; its offer is answered once that commits. */
    granted,
    /** The character already holds the row's pursuit, so its offer was answered some time ago. */
    alreadyHeld,
    /** The row should have granted and could not, so its offer still stands. */
    grantRefused,
};

/**
 * Settles one resolved vendor row, in the order a row's behaviours are tried.
 *
 * Both vendor opcodes end here. A row is a bounty roll, an exchange, or a grant, and which cannot
 * be read off the row itself: each is recognised by an authored rule keyed to the vendor, tried in
 * turn, and the first that claims the row owns it. One ordered chain is what keeps 901 and 904 from
 * drifting apart.
 *
 * @param message Request being answered.
 * @param opcode Opcode to report under.
 * @param vendorIndex Vendor the request names.
 * @param rowIndex Sale row the request names.
 * @param categoryIndex Category of that row, from sale row +100.
 * @param itemDefinitionIndex Item the row names.
 * @param outcome Receives whatever mutation the row prepared.
 * @return What the row turned out to be.
 */
RowOutcome settle_vendor_row(const middleware::web_service::Message& message,
                             std::uint16_t opcode,
                             std::int32_t vendorIndex,
                             std::int32_t rowIndex,
                             std::int32_t categoryIndex,
                             std::uint16_t itemDefinitionIndex,
                             Outcome& outcome) noexcept {
    std::uint16_t rolledBounty = kUnavailableDefinitionIndex;
    if (roll_vendor_bounty(vendorIndex, categoryIndex, rolledBounty)) {
        report_purchase(opcode,
                        "ok",
                        rolledBounty == kUnavailableDefinitionIndex ? "bounty_pool_empty"
                                                                   : "bounty_roll",
                        vendorIndex,
                        rowIndex,
                        itemDefinitionIndex);
        if (rolledBounty != kUnavailableDefinitionIndex) {
            std::uint16_t rolledCollectible = state::build_data::collectibles::kNoCollectibleIndex;
            (void)find_collectible_for_item(rolledBounty, rolledCollectible);
            (void)grant_item_definition(message, rolledCollectible, rolledBounty, outcome);
        }
        return RowOutcome::bountyRoll;
    }
    state::PendingProfileItemAcquisition exchange{};
    if (exchange_vendor_row(vendorIndex, rowIndex, exchange)) {
        report_purchase(opcode, "ok", "exchange", vendorIndex, rowIndex, itemDefinitionIndex);
        if (exchange.prepared) {
            outcome.mutation = exchange;
        }
        return RowOutcome::exchange;
    }
    // A placeholder row grants what it stands for, not the placeholder: a Dummy item put in the
    // Quests bucket is one the client will not draw, and the row never settles because the player
    // never receives what it offered.
    std::uint16_t granted = itemDefinitionIndex;
    std::uint16_t substituteIndex = kUnavailableDefinitionIndex;
    switch (substitute_for_item(granted, substituteIndex)) {
    case Substitution::replaced:
        granted = substituteIndex;
        break;
    case Substitution::broken:
        // The rule proves the row's item is a placeholder, so granting it would be the wrong
        // grant this path exists to prevent. The rule itself already logged what is missing.
        report_purchase(opcode, "fail", "substitute_missing", vendorIndex, rowIndex, granted);
        return RowOutcome::grantRefused;
    case Substitution::none:
        break;
    }
    std::uint16_t collectibleIndex = state::build_data::collectibles::kNoCollectibleIndex;
    const bool collected = find_collectible_for_item(granted, collectibleIndex);
    report_purchase(opcode,
                    "ok",
                    collected ? "resolved" : "resolved_no_collectible",
                    vendorIndex,
                    rowIndex,
                    granted);
    // A grant that failed for a transient reason - the loadout would not resolve, the bucket was
    // full - leaves the row's offer standing, and the caller must not treat it as answered.
    switch (grant_item_definition(message, collectibleIndex, granted, outcome)) {
    case GrantResult::granted:
        return RowOutcome::granted;
    case GrantResult::alreadyHeld:
        return RowOutcome::alreadyHeld;
    case GrantResult::refused:
        break;
    }
    return RowOutcome::grantRefused;
}

/**
 * Prepares one opcode-904 quest acquire.
 *
 * A quest names a vendor row exactly as a purchase does, and the item behind it is granted through
 * the same path, so a quest lands in the inventory the way a bounty now does.
 */
void acquire_quest(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace quest = middleware::web_service::messages::opcode904;
    quest::Request request{};
    if (!quest::parse_request(message, request)) {
        report_purchase(quest::kOpcode, "fail", "payload", -1, -1, kUnavailableDefinitionIndex);
        return;
    }
    // The 16-bit slot field is where the click landed, and indexing sale rows with it granted
    // armour mods. The 32-bit field is the real row; a body without one has never been captured,
    // and guessing the slot in as a sale row would reproduce that exact wrong grant - so it is
    // refused, and the refusal names the shape so a real capture can settle it.
    if (!request.hasSaleIndex) {
        report_purchase(quest::kOpcode,
                        "fail",
                        "sale_field_missing",
                        request.vendorIndex,
                        request.slotIndex,
                        kUnavailableDefinitionIndex);
        return;
    }
    const std::int32_t row = request.saleIndex;
    std::uint16_t itemDefinitionIndex = 0;
    const char* reason = "unknown";
    // A row of -1 is the client saying this tile is not a sale row at all, rather than a row that
    // failed to resolve, so it takes the installed array instead. Falling back to the slot as a
    // sale row would grant whatever sits there, which is the wrong-item bug that made quests hand
    // out armour mods.
    const bool rowless = row < 0;
    // A rowless 904 is an interaction reply rather than a purchase, and the rank-up reward tile is
    // one: its reply names no sale row, so the slot field is the interaction it answered.
    std::int32_t questCategoryIndex = -1;
    const bool located =
        rowless ? resolve_rowless_quest(request.vendorIndex, request.slotIndex, itemDefinitionIndex)
                : resolve_vendor_row(request.vendorIndex, row, itemDefinitionIndex,
                                    questCategoryIndex, reason);
    if (!located) {
        report_purchase(quest::kOpcode,
                        "fail",
                        rowless ? "rowless_unresolved" : reason,
                        request.vendorIndex,
                        row,
                        kUnavailableDefinitionIndex);
        // A tile that names no row grants nothing, so say what this vendor does offer that would
        // land in the Quests tab. That is the difference between "this click is broken" and "this
        // click was never a quest".
        if (rowless) {
            report_pursuit_rows(request.vendorIndex);
        }
        return;
    }
    const RowOutcome settled = settle_vendor_row(message,
                                                 quest::kOpcode,
                                                 request.vendorIndex,
                                                 row,
                                                 questCategoryIndex,
                                                 itemDefinitionIndex,
                                                 outcome);
    // The banner that offered this quest is answered only by a row whose offer is answered, and
    // nothing else tells the client so: its picker keeps choosing the same interaction for as long
    // as the quest is offerable. A bounty roll and an exchange leave the banner's own question
    // unanswered, and a refused grant still owes the player its quest.
    if (request.vendorIndex < 0
        || request.vendorIndex >= static_cast<std::int32_t>(state::vendors::kVendorCapacity)) {
        return;
    }
    const auto vendor = static_cast<std::uint16_t>(request.vendorIndex);
    switch (settled) {
    case RowOutcome::alreadyHeld:
        // Answered some time ago, and nothing is left to commit, so the banner retires now. This
        // is the re-click on a quest already in the tab.
        (void)state::vendors::answer_shown(vendor);
        break;
    case RowOutcome::granted:
        // Prepared, not committed. The answer rides the transaction and is written where the
        // grant commits, so a mutation dropped on the way never buries a quest still owed.
        outcome.answeredVendor = vendor;
        break;
    case RowOutcome::bountyRoll:
    case RowOutcome::exchange:
    case RowOutcome::grantRefused:
        break;
    }
}

/**
 * Prepares one opcode-901 vendor purchase, for any Tower vendor.
 *
 * The request names a vendor row and a sale row. The sale row names an item-definition index, which
 * is the same thing a Collections pull resolves its collectible to, so this resolves the row and
 * hands over to the very same grant.
 *
 * Cost is deliberately not charged: the sale row's cost-bearing fields are still role-open, and the
 * domain header warns against naming one a cost without its mutation reader.
 */
void purchase_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace purchase = middleware::web_service::messages::opcode901;
    purchase::Request request{};
    if (!purchase::parse_request(message, request)) {
        report_purchase(purchase::kOpcode, "fail", "payload", -1, -1, kUnavailableDefinitionIndex);
        return;
    }
    std::uint16_t itemDefinitionIndex = 0;
    const char* reason = "unknown";
    std::int32_t categoryIndex = -1;
    if (!resolve_vendor_row(
            request.vendorIndex, request.saleIndex, itemDefinitionIndex, categoryIndex, reason)) {
        report_purchase(purchase::kOpcode,
                        "fail",
                        reason,
                        request.vendorIndex,
                        request.saleIndex,
                        kUnavailableDefinitionIndex);
        return;
    }
    // Bounties, quest steps and tokens carry no collectible. The acquisition takes the sentinel
    // rather than a made-up row, and both prepare and commit skip the collectible steps for it.
    (void)settle_vendor_row(message,
                            purchase::kOpcode,
                            request.vendorIndex,
                            request.saleIndex,
                            categoryIndex,
                            itemDefinitionIndex,
                            outcome);
}

} // namespace sunrise::server::web_service
