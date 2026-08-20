#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

#include "client/content/items/packages/package_socket_plug_build.h"
#include "middleware/content/packages/tables/definition_index_table.h"
#include "state/account/inventory/item_state.h"
#include "state/build_data/cache/records/codec.h"
#include "state/build_data/items/catalysts/exotic_catalyst_builder.h"
#include "state/build_data/items/catalysts/exotic_catalyst_catalog.h"

extern int failures;

namespace {

namespace catalysts = sunrise::state::build_data::items::catalysts;
namespace cache_records = sunrise::state::build_data::cache::records;
namespace package_items = sunrise::client::content::items::packages;
namespace package_tables = sunrise::middleware::content::packages::tables;
namespace details = sunrise::state::build_data::items::details;
namespace items = sunrise::state::build_data::items;
namespace socket_plugs = sunrise::state::build_data::items::socket_plugs;

constexpr std::uint32_t kTimestamp = 0x5F43138BU;
constexpr std::uint32_t kImageSize = 0x08A5EA00U;
constexpr std::uint32_t kEmptyCatalyst = 0x5957A904U;
constexpr std::uint32_t kWorldline = 0x6F22FCECU;
constexpr std::uint32_t kWorldlineDefault = 0x9B283E92U;
constexpr std::uint32_t kWorldlineProgress = 0x172766F5U;
constexpr std::uint32_t kWorldlineComplete = 0x61935831U;
constexpr std::uint32_t kWorldlineEffect = 0x9F651900U;
constexpr std::uint32_t kWorldlineCategory = 0x06989D65U;
constexpr std::uint16_t kWorldlineSocketType = 447;
constexpr std::uint16_t kWorldlineAcquisition = 5044;
constexpr std::uint32_t kWorldlineAcquisitionHash = 0x12345678U;
constexpr std::uint16_t kWorldlineCompletionValue = 3167;
constexpr std::int32_t kWorldlineCompletionThreshold = 4;
constexpr std::uint32_t kWitherhoard = 0x8C8180D6U;
constexpr std::uint32_t kWitherhoardComplete = 0xAC29C6ACU;
constexpr std::uint32_t kWitherhoardCategory = 0x1234ABCDU;
constexpr std::uint16_t kWitherhoardSocketType = 727;
constexpr std::uint16_t kWitherhoardAcquisition = 11276;
constexpr std::uint32_t kWishEnder = 0x3092080CU;
constexpr std::uint32_t kWishEnderActive = 0xCE1AD8BBU;

void expect(bool value, const char* label) noexcept {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

template <typename Value>
void write_bytes(std::span<std::byte> output,
                 std::size_t offset,
                 const Value& value) noexcept {
    std::memcpy(output.data() + offset, &value, sizeof value);
}

void write_completion_expression(std::span<std::byte> output,
                                 std::size_t descriptorOffset,
                                 std::size_t headerOffset,
                                 std::uint32_t valueIndex,
                                 std::uint32_t value) noexcept {
    constexpr std::uint64_t kTokenCount = 3;
    constexpr std::uint32_t kHeaderMarker = 0x80800001U;
    const std::int64_t relative = static_cast<std::int64_t>(headerOffset)
                                  - static_cast<std::int64_t>(descriptorOffset + 8);
    write_bytes(output, descriptorOffset, kTokenCount);
    write_bytes(output, descriptorOffset + 8, relative);
    write_bytes(output, headerOffset - 4, kHeaderMarker);
    write_bytes(output, headerOffset, kTokenCount);
    write_bytes(output, headerOffset + 8, package_tables::kInvestmentExpressionRowClass);
    const std::array<std::uint32_t, 6> tokens{
        10, valueIndex, 11, value, 14, UINT32_MAX};
    std::memcpy(output.data() + headerOffset + 16, tokens.data(), sizeof tokens);
}

items::Definition item(std::uint16_t index,
                       std::uint32_t hash,
                       items::Tier tier = items::Tier::none) noexcept {
    items::Definition value{};
    value.definitionIndex = index;
    value.definitionHash = hash;
    value.tier = static_cast<std::uint8_t>(tier);
    return value;
}

details::Definition weapon_detail(std::uint16_t index,
                                  std::uint32_t hash,
                                  std::uint8_t lane,
                                  std::uint16_t defaultPlug,
                                  std::uint16_t socketType) noexcept {
    details::Definition value{};
    value.definitionIndex = index;
    value.definitionHash = hash;
    value.instancedDefinitionState = details::InstancedDefinitionState::instanced;
    value.equipmentSlot = std::int8_t{7};
    value.ordinarySocketState = details::OrdinarySocketState::present;
    value.ordinarySocketCount = static_cast<std::uint8_t>(lane + 1);
    value.initialPlugIndices[lane] = defaultPlug;
    value.socketTypes[lane] = socketType;
    return value;
}

struct Fixture {
    std::array<items::Definition, 6> itemRows{};
    std::array<details::Definition, 2> detailRows{};
    std::array<socket_plugs::Rule, 1> rules{{{10, 7, 0, 1}}};
    std::array<socket_plugs::Pool, 2> pools{{{0, 0}, {0, 3}}};
    std::array<socket_plugs::Member, 3> members{{20, 21, 22}};
    std::array<std::uint32_t, 1> legacy{{kWorldlineComplete}};
    std::array<std::uint32_t, 1> released{{kWorldline}};
    std::array<catalysts::CompletionCondition, 1> completionConditions{{{
        23,
        kWorldlineCompletionValue,
        kWorldlineCompletionThreshold,
        catalysts::CompletionConditionState::present,
    }}};
    std::array<catalysts::AcquisitionGate, 1> acquisitionGates{{{
        kWorldlineSocketType,
        kWorldlineAcquisition,
        catalysts::AcquisitionState::present,
    }}};
    std::array<catalysts::Definition, catalysts::kDefinitionCapacity> output{};

    Fixture() noexcept {
        itemRows = {item(10, kWorldline, items::Tier::exotic),
                    item(20, kWorldlineDefault),
                    item(21, kWorldlineProgress),
                    item(22, kWorldlineComplete),
                    item(23, kWorldlineEffect, items::Tier::exotic),
                    item(kWorldlineAcquisition, kWorldlineAcquisitionHash)};
        for (std::size_t index = 1; index < itemRows.size() - 1; ++index) {
            itemRows[index].plugCategoryHash = kWorldlineCategory;
        }
        detailRows[0] = weapon_detail(10, kWorldline, 7, 20, kWorldlineSocketType);
        detailRows[1].definitionIndex = 23;
        detailRows[1].definitionHash = kWorldlineEffect;
        detailRows[1].instancedDefinitionState =
            details::InstancedDefinitionState::stackable;
        detailRows[1].sandboxPerkCount = 1;
        detailRows[1].sandboxPerks[0] = 77;
    }

    [[nodiscard]] catalysts::Source source() const noexcept {
        return {{kTimestamp, kImageSize, 0},
                itemRows,
                detailRows,
                rules,
                pools,
                members,
                completionConditions,
                acquisitionGates};
    }

    [[nodiscard]] catalysts::Facts facts() const noexcept {
        return {kTimestamp, kImageSize, kEmptyCatalyst, legacy, released};
    }
};

void test_completion_expression_reader() noexcept {
    std::array<std::byte, 192> definition{};
    write_completion_expression(
        definition, 0, 64, kWorldlineCompletionValue, kWorldlineCompletionThreshold);
    catalysts::CompletionCondition condition{};
    package_items::read_catalyst_completion_condition(definition, 23, condition);
    expect(condition.itemDefinitionIndex == 23
               && condition.valueIndex == kWorldlineCompletionValue
               && condition.value == kWorldlineCompletionThreshold
               && condition.state == catalysts::CompletionConditionState::present,
           "native completion expression resolves");

    write_completion_expression(
        definition, 16, 128, kWorldlineCompletionValue, kWorldlineCompletionThreshold);
    package_items::read_catalyst_completion_condition(definition, 23, condition);
    expect(condition.state == catalysts::CompletionConditionState::present
               && condition.valueIndex == kWorldlineCompletionValue
               && condition.value == kWorldlineCompletionThreshold,
           "duplicate native completion expressions remain one condition");

    write_completion_expression(
        definition, 16, 128, kWorldlineCompletionValue - 1, kWorldlineCompletionThreshold);
    package_items::read_catalyst_completion_condition(definition, 23, condition);
    expect(condition.state == catalysts::CompletionConditionState::ambiguous
               && condition.valueIndex == catalysts::kUnavailableCompletionValueIndex
               && condition.value == 0,
           "distinct native completion expressions fail closed");

    definition = {};
    write_completion_expression(
        definition, 0, 64, kWorldlineCompletionValue, kWorldlineCompletionThreshold);
    constexpr std::uint32_t kWrongComparison = 13;
    write_bytes(definition, 64 + 16 + 4 * sizeof(std::uint32_t), kWrongComparison);
    package_items::read_catalyst_completion_condition(definition, 23, condition);
    expect(condition.state == catalysts::CompletionConditionState::absent,
           "non-completion postfix expressions stay absent");
}

void test_worldline_legacy_completion() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    expect(catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report),
           "Worldline catalog derives");
    expect(count == 1, "Worldline catalog has one record");
    expect(report.released == 1 && report.placeholder == 0 && report.unsupported == 0,
           "Worldline report is released");
    expect(catalysts::matches_derived(
               fixture.source(), fixture.facts(), std::span(fixture.output).first(count)),
           "Worldline stored catalog matches fresh derivation");
    if (count != 1) {
        return;
    }
    const catalysts::Definition& record = fixture.output[0];
    expect(record.itemDefinitionHash == kWorldline, "Worldline record keeps weapon hash");
    expect(record.itemDefinitionIndex == 10, "Worldline record keeps weapon index");
    expect(record.socketLane == 7, "Worldline resolves lane 7");
    expect(record.completedPlugDefinitionIndex == 22, "Worldline resolves completed plug");
    expect(record.effectDefinitionIndex == 23, "Worldline resolves actual catalyst effect");
    expect(record.acquisitionDefinitionIndex == kWorldlineAcquisition,
           "Worldline resolves its acquired-state gate");
    expect(record.completionValueIndex == kWorldlineCompletionValue
               && record.completionValue == kWorldlineCompletionThreshold,
           "Worldline resolves its native completion condition");
    expect(record.availability == catalysts::Availability::released,
           "Worldline is released");

    catalysts::clear();
    expect(catalysts::replace(std::span(fixture.output).first(count)),
           "Worldline catalog publishes");
    expect(catalysts::resolve_effect(10, 7, 22) == 23,
           "Worldline display plug resolves actual catalyst effect");
    expect(catalysts::resolve_effect(10, 7, 20) == 20,
           "Worldline default plug has no catalyst effect");
    for (std::uint32_t flags = 0; flags <= 3; ++flags) {
        std::array<std::optional<std::uint16_t>, 12> plugs{};
        plugs[7] = std::uint16_t{20};
        std::uint32_t changedFlags = flags;
        expect(catalysts::apply_completed(10, changedFlags, plugs)
                   == catalysts::ApplyResult::completed,
               "Worldline completion applies");
        expect(changedFlags == (flags | sunrise::state::account::inventory::kMasterworkItemFlag),
               "Worldline completion preserves item-state bits");
        expect(plugs[7] == 23, "Worldline completion applies native effect plug with flag");
    }
    catalysts::set_completion_enabled(false);
    std::array<std::optional<std::uint16_t>, 12> disabledPlugs{};
    disabledPlugs[7] = std::uint16_t{20};
    const auto disabledBefore = disabledPlugs;
    std::uint32_t disabledFlags = 3;
    expect(catalysts::apply_completed(10, disabledFlags, disabledPlugs)
               == catalysts::ApplyResult::unchanged,
           "global policy can disable catalyst completion");
    expect(disabledFlags == 3 && disabledPlugs == disabledBefore,
           "disabled catalyst completion leaves item unchanged");
    catalysts::set_completion_enabled(true);
}

void test_investment_completion_is_atomic() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    if (!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
        || !catalysts::replace(std::span(fixture.output).first(count))) {
        expect(false, "investment fixture publishes");
        return;
    }

    sunrise::state::Family5State family{};
    family.objectSoid = 0x4000000000000005ULL;
    family.contentGateArm = true;
    family.flags[0] = {77, 1};
    family.flags[1] = {kWorldlineAcquisition, 0};
    family.flags[2] = {kWorldlineAcquisition, 1};
    family.flagCount = 3;
    family.values[0] = {88, 7};
    family.values[1] = {kWorldlineCompletionValue, 2};
    family.values[2] = {kWorldlineCompletionValue, 9};
    family.valueCount = 3;

    expect(catalysts::append_investment_overrides(family),
           "Worldline investment state completes");
    expect(family.objectSoid == 0x4000000000000005ULL && family.contentGateArm,
           "catalyst completion preserves Family 5 identity and arm state");
    expect(family.flagCount == 2 && family.flags[0].slot == 77
               && family.flags[0].value == 1
               && family.flags[1].slot == kWorldlineAcquisition
               && family.flags[1].value == 2,
           "catalyst completion deduplicates and sets its acquired-state gate");
    expect(family.valueCount == 2 && family.values[0].slot == 88
               && family.values[0].value == 7
               && family.values[1].slot == kWorldlineCompletionValue
               && family.values[1].value == 9,
           "catalyst completion deduplicates without lowering authored values");

    catalysts::set_completion_enabled(false);
    const std::size_t disabledFlagCount = family.flagCount;
    const std::size_t disabledValueCount = family.valueCount;
    const auto disabledFlags = family.flags;
    const auto disabledValues = family.values;
    expect(catalysts::append_investment_overrides(family),
           "disabled catalyst investment completion succeeds without changes");
    expect(family.flagCount == disabledFlagCount && family.valueCount == disabledValueCount
               && family.flags[0].slot == disabledFlags[0].slot
               && family.flags[0].value == disabledFlags[0].value
               && family.values[0].slot == disabledValues[0].slot
               && family.values[0].value == disabledValues[0].value,
           "disabled catalyst investment completion leaves authored state unchanged");
    catalysts::set_completion_enabled(true);

    sunrise::state::Family5State full{};
    full.flags[0] = {kWorldlineAcquisition, 0};
    full.flagCount = 1;
    full.valueCount = full.values.size();
    for (std::size_t index = 0; index < full.valueCount; ++index) {
        full.values[index] = {static_cast<std::uint16_t>(index),
                              static_cast<std::int32_t>(index)};
    }
    const auto fullValues = full.values;
    expect(!catalysts::append_investment_overrides(full),
           "full completion-value bank rejects catalyst completion");
    expect(full.flagCount == 1 && full.flags[0].value == 0
               && full.valueCount == full.values.size()
               && full.values[full.valueCount - 1].slot
                      == fullValues[full.valueCount - 1].slot
               && full.values[full.valueCount - 1].value
                      == fullValues[full.valueCount - 1].value,
           "failed investment completion rolls back both Family 5 banks");
}

void test_later_two_plug_and_placeholder() noexcept {
    std::array<items::Definition, 3> itemRows{
        item(10, kWitherhoard, items::Tier::exotic),
        item(20, kEmptyCatalyst),
        item(21, kWitherhoardComplete),
    };
    itemRows[1].plugCategoryHash = kWitherhoardCategory;
    itemRows[2].plugCategoryHash = kWitherhoardCategory;
    itemRows[2].tier = static_cast<std::uint8_t>(items::Tier::exotic);
    std::array<details::Definition, 2> detailRows{
        weapon_detail(10, kWitherhoard, 10, 20, kWitherhoardSocketType),
        details::Definition{},
    };
    detailRows[1].definitionIndex = 21;
    detailRows[1].definitionHash = kWitherhoardComplete;
    detailRows[1].instancedDefinitionState = details::InstancedDefinitionState::stackable;
    detailRows[1].sandboxPerkCount = 1;
    detailRows[1].sandboxPerks[0] = 88;
    std::array<socket_plugs::Rule, 1> rules{{{10, 10, 0, 1}}};
    std::array<socket_plugs::Pool, 2> pools{{{0, 0}, {0, 2}}};
    std::array<socket_plugs::Member, 2> members{{20, 21}};
    std::array<std::uint32_t, 1> released{{kWitherhoard}};
    std::array<catalysts::AcquisitionGate, 1> acquisitionGates{{{
        kWitherhoardSocketType,
        kWitherhoardAcquisition,
        catalysts::AcquisitionState::present,
    }}};
    std::array<catalysts::Definition, catalysts::kDefinitionCapacity> output{};
    const catalysts::Facts facts{kTimestamp, kImageSize, kEmptyCatalyst, {}, released};
    const catalysts::Source source{
        {kTimestamp, kImageSize, 0},
        itemRows,
        detailRows,
        rules,
        pools,
        members,
        {},
        acquisitionGates};
    std::size_t count = 0;
    catalysts::Report report{};
    expect(catalysts::derive(source, facts, output, count, report),
           "Witherhoard catalog derives");
    expect(count == 1 && output[0].socketLane == 10
               && output[0].completedPlugDefinitionIndex == 21
               && output[0].effectDefinitionIndex == 21
               && output[0].acquisitionDefinitionIndex == kWitherhoardAcquisition
               && output[0].completionValueIndex
                      == catalysts::kUnavailableCompletionValueIndex,
           "Witherhoard resolves later lifecycle");

    itemRows[0].definitionHash = kWishEnder;
    itemRows[2].definitionHash = kWishEnderActive;
    detailRows[0].definitionHash = kWishEnder;
    detailRows[1].definitionHash = kWishEnderActive;
    const catalysts::Facts placeholderFacts{kTimestamp, kImageSize, kEmptyCatalyst, {}, {}};
    count = 0;
    report = {};
    expect(catalysts::derive(source, placeholderFacts, output, count, report),
           "Wish-Ender placeholder catalog derives");
    expect(count == 1 && output[0].availability == catalysts::Availability::placeholder,
           "Wish-Ender stays a placeholder");
    expect(catalysts::replace(std::span(output).first(count)),
           "Wish-Ender placeholder publishes");
    const catalysts::Result result = catalysts::resolve(10);
    expect(result.error == catalysts::Error::placeholderOnly,
           "Wish-Ender does not resolve a completed catalyst");
    std::array<std::optional<std::uint16_t>, 12> plugs{};
    plugs[10] = std::uint16_t{20};
    const auto before = plugs;
    std::uint32_t flags = 3;
    expect(catalysts::apply_completed(10, flags, plugs) == catalysts::ApplyResult::unchanged,
           "Wish-Ender completion is skipped");
    expect(flags == 3 && plugs == before, "Wish-Ender state stays unchanged");
}

void test_exclusions_and_errors() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};

    fixture.itemRows[0].tier = static_cast<std::uint8_t>(items::Tier::legendary);
    const std::array<std::uint32_t, 0> noReleased{};
    const catalysts::Facts noReleaseFacts{
        kTimestamp, kImageSize, kEmptyCatalyst, fixture.legacy, noReleased};
    expect(catalysts::derive(
               fixture.source(), noReleaseFacts, fixture.output, count, report)
               && count == 0,
           "non-exotic weapon is excluded");

    fixture.itemRows[0].tier = static_cast<std::uint8_t>(items::Tier::exotic);
    fixture.detailRows[0].equipmentSlot = std::int8_t{10};
    count = 0;
    report = {};
    expect(catalysts::derive(
               fixture.source(), noReleaseFacts, fixture.output, count, report)
               && count == 0,
           "exotic armor is excluded");

    fixture.detailRows[0].equipmentSlot = std::int8_t{7};
    fixture.itemRows[2].definitionHash = kWorldlineComplete;
    fixture.legacy = {kWorldlineComplete};
    count = 0;
    report = {};
    expect(!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::ambiguousLifecycle,
           "unclear legacy lifecycle fails without partial records");

    fixture.itemRows[2].definitionHash = kWorldlineProgress;
    fixture.members = {21, 22, 10};
    count = 0;
    report = {};
    expect(!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::invalidPlug,
           "default plug outside its pool fails");

    fixture.members = {20, 21, 22};
    fixture.itemRows[4].tier = static_cast<std::uint8_t>(items::Tier::none);
    count = 0;
    report = {};
    expect(!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::invalidEffect,
           "missing legacy catalyst effect fails");
    fixture.itemRows[4].tier = static_cast<std::uint8_t>(items::Tier::exotic);

    fixture.acquisitionGates[0].state = catalysts::AcquisitionState::ambiguous;
    count = 0;
    report = {};
    expect(!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::invalidAcquisition,
           "unclear acquired-state gate fails a released catalyst");
    fixture.acquisitionGates[0].state = catalysts::AcquisitionState::present;

    fixture.completionConditions[0].state = catalysts::CompletionConditionState::ambiguous;
    count = 0;
    report = {};
    expect(!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::invalidCompletion,
           "unclear completion expression fails a released catalyst");
    fixture.completionConditions[0].state = catalysts::CompletionConditionState::present;

    catalysts::Source wrongBuild = fixture.source();
    wrongBuild.build.imageTimestamp ^= 1U;
    count = 0;
    report = {};
    expect(!catalysts::derive(wrongBuild, fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::unsupportedBuild,
           "build fingerprint mismatch fails");
}

void test_unclear_placeholder_is_safe() noexcept {
    Fixture fixture;
    fixture.itemRows[2].definitionHash = kWorldlineComplete;
    const std::array<std::uint32_t, 0> noReleased{};
    const catalysts::Facts facts{
        kTimestamp, kImageSize, kEmptyCatalyst, fixture.legacy, noReleased};
    std::size_t count = 0;
    catalysts::Report report{};
    expect(catalysts::derive(fixture.source(), facts, fixture.output, count, report),
           "unclear placeholder does not abort the catalog");
    expect(count == 1 && report.unsupported == 1 && report.error == catalysts::Error::none,
           "unclear placeholder is reported as unsupported");
    if (count != 1) {
        return;
    }
    expect(fixture.output[0].availability == catalysts::Availability::unsupported
               && fixture.output[0].completedPlugDefinitionIndex
                      == details::kUnavailableItemIndex
               && fixture.output[0].effectDefinitionIndex
                      == details::kUnavailableItemIndex,
           "unclear placeholder has no completed state");
    expect(catalysts::replace(std::span(fixture.output).first(count)),
           "unsupported placeholder catalog publishes");
    std::array<std::optional<std::uint16_t>, 12> plugs{};
    plugs[7] = std::uint16_t{20};
    const auto before = plugs;
    std::uint32_t flags = 3;
    expect(catalysts::apply_completed(10, flags, plugs) == catalysts::ApplyResult::unchanged,
           "unsupported placeholder is skipped");
    expect(flags == 3 && plugs == before, "unsupported placeholder stays unchanged");
}

void test_cache_record_round_trip() noexcept {
    const catalysts::Definition released{
        kWorldline,
        10,
        22,
        23,
        kWorldlineAcquisition,
        kWorldlineCompletionValue,
        7,
        catalysts::Availability::released,
        kWorldlineCompletionThreshold};
    cache_records::ExoticCatalystRecord record{};
    catalysts::Definition decoded{};
    expect(cache_records::encode(released, record), "released catalyst cache record encodes");
    expect(cache_records::decode(record, decoded), "released catalyst cache record decodes");
    expect(decoded.itemDefinitionHash == released.itemDefinitionHash
               && decoded.itemDefinitionIndex == released.itemDefinitionIndex
               && decoded.completedPlugDefinitionIndex
                      == released.completedPlugDefinitionIndex
               && decoded.effectDefinitionIndex == released.effectDefinitionIndex
               && decoded.acquisitionDefinitionIndex
                      == released.acquisitionDefinitionIndex
               && decoded.completionValueIndex == released.completionValueIndex
               && decoded.socketLane == released.socketLane
               && decoded.availability == released.availability
               && decoded.completionValue == released.completionValue,
           "released catalyst cache record round-trips");

    const catalysts::Definition unsupported{kWishEnder,
                                             11,
                                             details::kUnavailableItemIndex,
                                             details::kUnavailableItemIndex,
                                             catalysts::kUnavailableAcquisitionIndex,
                                             catalysts::kUnavailableCompletionValueIndex,
                                             10,
                                             catalysts::Availability::unsupported,
                                             0};
    expect(cache_records::encode(unsupported, record),
           "unsupported catalyst cache record encodes");
    expect(cache_records::decode(record, decoded)
               && decoded.availability == catalysts::Availability::unsupported
               && decoded.completedPlugDefinitionIndex == details::kUnavailableItemIndex
               && decoded.effectDefinitionIndex == details::kUnavailableItemIndex,
           "unsupported catalyst cache record round-trips");
    record.availability = 0xFFU;
    expect(!cache_records::decode(record, decoded),
           "noncanonical catalyst cache record is rejected");
    expect(cache_records::kCacheFormatVersion == 47,
           "catalyst records use the next cache format");
}

void test_cached_catalog_revalidation() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    if (!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)) {
        expect(false, "cache validation fixture derives");
        return;
    }
    catalysts::Source cachedSource = fixture.source();
    cachedSource.completionConditions = {};
    cachedSource.acquisitionGates = {};
    expect(catalysts::matches_cached(cachedSource,
                                     fixture.facts(),
                                     std::span(fixture.output).first(count)),
           "cached catalyst catalog re-derives from stored relations");

    fixture.output[0].acquisitionDefinitionIndex = kWorldlineAcquisition - 1;
    expect(!catalysts::matches_cached(cachedSource,
                                      fixture.facts(),
                                      std::span(fixture.output).first(count)),
           "cached catalyst catalog rejects an absent acquisition item");
}

void test_atomic_rollback() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    if (!catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report)
        || !catalysts::replace(std::span(fixture.output).first(count))) {
        expect(false, "rollback fixture publishes");
        return;
    }

    std::array<std::optional<std::uint16_t>, 12> plugs{};
    plugs[7] = std::uint16_t{20};
    const auto before = plugs;
    std::uint32_t flags = 0x8U;
    expect(catalysts::apply_completed(10, flags, plugs) == catalysts::ApplyResult::failed,
           "invalid item state rejects completion");
    expect(flags == 0x8U && plugs == before, "invalid item state rolls back both fields");

    std::array<std::optional<std::uint16_t>, 7> shortPlugs{};
    const auto shortBefore = shortPlugs;
    flags = 3;
    expect(catalysts::apply_completed(10, flags, shortPlugs) == catalysts::ApplyResult::failed,
           "missing catalyst lane rejects completion");
    expect(flags == 3 && shortPlugs == shortBefore, "missing lane rolls back both fields");
}

} // namespace

void test_exotic_catalysts() noexcept {
    test_completion_expression_reader();
    test_worldline_legacy_completion();
    test_investment_completion_is_atomic();
    test_later_two_plug_and_placeholder();
    test_exclusions_and_errors();
    test_unclear_placeholder_is_safe();
    test_atomic_rollback();
    test_cache_record_round_trip();
    test_cached_catalog_revalidation();
    catalysts::clear();
}
