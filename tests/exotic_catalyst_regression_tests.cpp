#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>

#include "state/account/inventory/item_state.h"
#include "state/build_data/cache/records/codec.h"
#include "state/build_data/items/catalysts/exotic_catalyst_builder.h"
#include "state/build_data/items/catalysts/exotic_catalyst_catalog.h"

extern int failures;

namespace {

namespace catalysts = sunrise::state::build_data::items::catalysts;
namespace cache_records = sunrise::state::build_data::cache::records;
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
constexpr std::uint32_t kWitherhoard = 0x8C8180D6U;
constexpr std::uint32_t kWitherhoardComplete = 0xAC29C6ACU;
constexpr std::uint32_t kWitherhoardCategory = 0x1234ABCDU;
constexpr std::uint32_t kWishEnder = 0x3092080CU;
constexpr std::uint32_t kWishEnderActive = 0xCE1AD8BBU;

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
    value.definitionIndex = index;
    value.definitionHash = hash;
    value.tier = static_cast<std::uint8_t>(tier);
    return value;
}

details::Definition weapon_detail(std::uint16_t index,
                                  std::uint32_t hash,
                                  std::uint8_t lane,
                                  std::uint16_t defaultPlug) noexcept {
    details::Definition value{};
    value.definitionIndex = index;
    value.definitionHash = hash;
    value.instancedDefinitionState = details::InstancedDefinitionState::instanced;
    value.equipmentSlot = std::int8_t{7};
    value.ordinarySocketState = details::OrdinarySocketState::present;
    value.ordinarySocketCount = static_cast<std::uint8_t>(lane + 1);
    value.initialPlugIndices[lane] = defaultPlug;
    return value;
}

struct Fixture {
    std::array<items::Definition, 5> itemRows{};
    std::array<details::Definition, 2> detailRows{};
    std::array<socket_plugs::Rule, 1> rules{{{10, 7, 0, 1}}};
    std::array<socket_plugs::Pool, 2> pools{{{0, 0}, {0, 3}}};
    std::array<socket_plugs::Member, 3> members{{20, 21, 22}};
    std::array<std::uint32_t, 1> legacy{{kWorldlineComplete}};
    std::array<std::uint32_t, 1> released{{kWorldline}};
    std::array<catalysts::Definition, catalysts::kDefinitionCapacity> output{};

    Fixture() noexcept {
        itemRows = {item(10, kWorldline, items::Tier::exotic),
                    item(20, kWorldlineDefault),
                    item(21, kWorldlineProgress),
                    item(22, kWorldlineComplete),
                    item(23, kWorldlineEffect, items::Tier::exotic)};
        for (std::size_t index = 1; index < itemRows.size(); ++index) {
            itemRows[index].plugCategoryHash = kWorldlineCategory;
        }
        detailRows[0] = weapon_detail(10, kWorldline, 7, 20);
        detailRows[1].definitionIndex = 23;
        detailRows[1].definitionHash = kWorldlineEffect;
        detailRows[1].instancedDefinitionState =
            details::InstancedDefinitionState::stackable;
        detailRows[1].sandboxPerkCount = 1;
        detailRows[1].sandboxPerks[0] = 77;
    }

    [[nodiscard]] catalysts::Source source() const noexcept {
        return {{kTimestamp, kImageSize, 0}, itemRows, detailRows, rules, pools, members};
    }

    [[nodiscard]] catalysts::Facts facts() const noexcept {
        return {kTimestamp, kImageSize, kEmptyCatalyst, legacy, released};
    }
};

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
        expect(plugs[7] == 22, "Worldline completion applies plug with flag");
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
        weapon_detail(10, kWitherhoard, 10, 20),
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
    std::array<catalysts::Definition, catalysts::kDefinitionCapacity> output{};
    const catalysts::Facts facts{kTimestamp, kImageSize, kEmptyCatalyst, {}, released};
    const catalysts::Source source{
        {kTimestamp, kImageSize, 0}, itemRows, detailRows, rules, pools, members};
    std::size_t count = 0;
    catalysts::Report report{};
    expect(catalysts::derive(source, facts, output, count, report),
           "Witherhoard catalog derives");
    expect(count == 1 && output[0].socketLane == 10
               && output[0].completedPlugDefinitionIndex == 21
               && output[0].effectDefinitionIndex == 21,
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
        kWorldline, 10, 22, 23, 7, catalysts::Availability::released, 0};
    cache_records::ExoticCatalystRecord record{};
    catalysts::Definition decoded{};
    expect(cache_records::encode(released, record), "released catalyst cache record encodes");
    expect(cache_records::decode(record, decoded), "released catalyst cache record decodes");
    expect(decoded.itemDefinitionHash == released.itemDefinitionHash
               && decoded.itemDefinitionIndex == released.itemDefinitionIndex
               && decoded.completedPlugDefinitionIndex
                      == released.completedPlugDefinitionIndex
               && decoded.effectDefinitionIndex == released.effectDefinitionIndex
               && decoded.socketLane == released.socketLane
               && decoded.availability == released.availability && decoded.reserved == 0,
           "released catalyst cache record round-trips");

    const catalysts::Definition unsupported{kWishEnder,
                                             11,
                                             details::kUnavailableItemIndex,
                                             details::kUnavailableItemIndex,
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
    record.reserved = 1;
    expect(!cache_records::decode(record, decoded),
           "noncanonical catalyst cache record is rejected");
    expect(cache_records::kCacheFormatVersion == 46,
           "catalyst records use the next cache format");
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
    test_worldline_legacy_completion();
    test_later_two_plug_and_placeholder();
    test_exclusions_and_errors();
    test_unclear_placeholder_is_safe();
    test_atomic_rollback();
    test_cache_record_round_trip();
    catalysts::clear();
}
