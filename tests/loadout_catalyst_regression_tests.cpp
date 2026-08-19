#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>

#include "middleware/datagen/character_record/appearance/internal.h"
#include "middleware/datagen/family4/loadout/loadout_item_resolver.h"
#include "state/build_data/inventory/buckets/inventory_bucket_catalog.h"
#include "state/build_data/items/catalysts/exotic_catalyst_catalog.h"
#include "state/build_data/items/details/item_detail_catalog.h"
#include "state/build_data/items/item_catalog.h"
#include "state/build_data/runtime.h"
#include "state/build_data/socket_entry_lists/socket_entry_list_catalog.h"

extern int failures;

namespace sunrise::state::build_data {

bool find_item_definition_hash(std::uint32_t definitionHash,
                               items::Definition& definition) noexcept {
    return items::find_hash(definitionHash, definition);
}

bool find_configured_item_detail(std::uint16_t definitionIndex,
                                 items::details::Definition& definition) noexcept {
    return items::details::find(definitionIndex, definition);
}

bool find_inventory_bucket_descriptor(
    std::uint8_t bucketId,
    inventory::buckets::Descriptor& descriptor) noexcept {
    return inventory::buckets::find(bucketId, descriptor);
}

bool find_socket_entry_list(std::uint16_t definitionIndex,
                            socket_entry_lists::Definition& definition) noexcept {
    return socket_entry_lists::find(definitionIndex, definition);
}

bool find_socket_entry_table(std::uint16_t definitionIndex,
                             socket_entry_lists::EntryTable& table) noexcept {
    return socket_entry_lists::find_entry_table(definitionIndex, table);
}

items::catalysts::ApplyResult complete_exotic_catalyst(
    std::uint16_t itemDefinitionIndex,
    std::uint32_t& flags,
    std::span<std::optional<std::uint16_t>> plugs) noexcept {
    return items::catalysts::apply_completed(itemDefinitionIndex, flags, plugs);
}

bool find_investment_constants(constants::InvestmentConstants& value) noexcept {
    value = {.extracted = true,
             .lightStatRow = 1,
             .characterStatRows = {2, 3, 4, 5, 6, 7}};
    return true;
}

std::uint16_t resolve_exotic_catalyst_effect(std::uint16_t itemDefinitionIndex,
                                             std::uint8_t socketLane,
                                             std::uint16_t plugDefinitionIndex) noexcept {
    return items::catalysts::resolve_effect(
        itemDefinitionIndex, socketLane, plugDefinitionIndex);
}

} // namespace sunrise::state::build_data

namespace {

namespace buckets = sunrise::state::build_data::inventory::buckets;
namespace catalysts = sunrise::state::build_data::items::catalysts;
namespace details = sunrise::state::build_data::items::details;
namespace items = sunrise::state::build_data::items;
namespace loadout = sunrise::middleware::datagen::family4::loadout;
namespace appearance = sunrise::middleware::datagen::character_record::appearance;
namespace character_layout = sunrise::middleware::datagen::character_record::layout;
namespace socket_lists = sunrise::state::build_data::socket_entry_lists;

constexpr std::uint32_t kWeaponHash = 0x12345678U;
constexpr std::uint32_t kDefaultPlugHash = 0x23456789U;
constexpr std::uint32_t kProgressPlugHash = 0x3456789AU;
constexpr std::uint32_t kCompletedPlugHash = 0x456789ABU;
constexpr std::uint32_t kEffectHash = 0x56789ABCU;
constexpr std::uint32_t kCatalystCategory = 0x6789ABCDU;
constexpr std::uint16_t kCatalystPerk = 77;
constexpr std::uint8_t kCatalystStatRow = 24;
constexpr std::int32_t kCatalystStatValue = 7;

void expect(bool value, const char* label) noexcept {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

items::Definition item(std::uint16_t index,
                       std::uint32_t hash,
                       items::Tier tier = items::Tier::none) noexcept {
    items::Definition value{};
    value.definitionHash = hash;
    value.definitionIndex = index;
    value.tier = static_cast<std::uint8_t>(tier);
    return value;
}

void clear_catalogs() noexcept {
    catalysts::clear();
    socket_lists::clear();
    buckets::clear();
    details::clear();
    items::clear();
}

} // namespace

void test_resolved_catalyst_output() noexcept {
    clear_catalogs();
    std::array<items::Definition, 5> itemRows{
        item(0, kWeaponHash, items::Tier::exotic),
        item(1, kDefaultPlugHash),
        item(2, kProgressPlugHash),
        item(3, kCompletedPlugHash),
        item(4, kEffectHash, items::Tier::exotic),
    };
    itemRows[0].bucketId = 0;
    itemRows[3].plugCategoryHash = kCatalystCategory;
    itemRows[4].plugCategoryHash = kCatalystCategory;

    details::Definition detail{};
    detail.definitionIndex = 0;
    detail.definitionHash = kWeaponHash;
    detail.bucketId = 0;
    detail.maxStackSize = 1;
    detail.instancedDefinitionState = details::InstancedDefinitionState::instanced;
    detail.equipmentSlot = std::int8_t{7};
    detail.ordinarySocketState = details::OrdinarySocketState::present;
    detail.ordinarySocketCount = 8;
    detail.initialPlugIndices[7] = 1;
    detail.socketEntryListIndex = 0;
    details::Definition effectDetail{};
    effectDetail.definitionIndex = 4;
    effectDetail.definitionHash = kEffectHash;
    effectDetail.bucketId = 0;
    effectDetail.maxStackSize = 1;
    effectDetail.instancedDefinitionState = details::InstancedDefinitionState::stackable;
    effectDetail.sandboxPerkCount = 1;
    effectDetail.sandboxPerks[0] = kCatalystPerk;
    effectDetail.statCount = 1;
    effectDetail.stats[0] = {kCatalystStatRow, kCatalystStatValue};
    const std::array detailRows{detail, effectDetail};

    const std::array bucketRows{
        buckets::Descriptor{0, buckets::ArraySelector::character, 0, 10, 7, 0},
    };
    const std::array socketListRows{
        socket_lists::Definition{0x12345678U, 0, 0, 0},
    };
    const std::array catalystRows{
        catalysts::Definition{kWeaponHash, 0, 3, 4, 7, catalysts::Availability::released, 0},
    };

    const bool ready = items::replace(itemRows) && details::replace(detailRows)
                       && buckets::replace(bucketRows)
                       && socket_lists::replace(socketListRows)
                       && catalysts::replace(catalystRows);
    expect(ready, "resolved-output fixture publishes");
    if (!ready) {
        clear_catalogs();
        return;
    }

    sunrise::state::account::inventory::Item authored{};
    authored.instanceSoid = 1;
    authored.definitionHash = kWeaponHash;
    authored.level = 50;
    authored.quantity = 1;
    authored.flags = 3;
    sunrise::state::CharacterState character{};
    loadout::Candidate output{};
    expect(loadout::resolve_item(
               authored, character, itemRows.size(), socketListRows.size(), output),
           "resolved client item accepts catalyst completion");
    expect(output.item.flags == 7, "resolved client item carries all item-state bits");
    expect(output.item.instance.ordinarySockets.plugs[7] == 3,
           "resolved client item carries the completed catalyst plug");

    loadout::ResolvedInstances instances{};
    instances.itemCount = 1;
    instances.items[0].equipmentSlot = 7;
    instances.items[0].instance = output.item.instance;
    character_layout::Appearance appearanceRecord{};
    appearanceRecord.smallBankA.fill(character_layout::kEmptyDefinitionIndex);
    appearanceRecord.overflowHashes.fill(character_layout::kNoHash);
    appearance::apply_perk_banks(instances, appearanceRecord);
    expect(appearanceRecord.smallBankA[0] == kCatalystPerk,
           "Worldline display plug publishes the actual catalyst perk");
    appearance::apply_overflow_hashes(instances, appearanceRecord);
    expect(appearanceRecord.overflowHashes[0] == kEffectHash,
           "Worldline display plug publishes the actual catalyst hash");
    expect(appearance::apply_stats(instances, 50, appearanceRecord),
           "Worldline display plug builds catalyst stats");
    expect(appearanceRecord.weaponStats[0][0].key
                   == static_cast<std::int8_t>(kCatalystStatRow)
               && appearanceRecord.weaponStats[0][0].value == kCatalystStatValue,
           "Worldline display plug publishes the actual catalyst stat");

    authored.flags = 0x8U;
    output.item.flags = 0xA5A5U;
    expect(!loadout::resolve_item(
               authored, character, itemRows.size(), socketListRows.size(), output),
           "resolved client item rejects an invalid accumulated state");
    expect(output.item.flags == 0xA5A5U,
           "failed resolved client item leaves caller output unchanged");
    clear_catalogs();
}
