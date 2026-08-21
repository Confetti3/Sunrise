#include <algorithm>
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
#include "state/build_data/runtime/persistence/build_data_persistence.h"

extern int failures;

namespace {

namespace cache_records = sunrise::state::build_data::cache::records;
namespace catalysts = sunrise::state::build_data::items::catalysts;
namespace details = sunrise::state::build_data::items::details;
namespace items = sunrise::state::build_data::items;
namespace persistence = sunrise::state::build_data::runtime::persistence;
namespace socket_plugs = sunrise::state::build_data::items::socket_plugs;

constexpr std::uint32_t kTimestamp = 0x12345678U;
constexpr std::uint32_t kImageSize = 0x01000000U;
constexpr std::uint32_t kLegacyWeaponHash = 0x10000010U;
constexpr std::uint32_t kLaterWeaponHash = 0x10000020U;
constexpr std::uint32_t kPlaceholderWeaponHash = 0x10000030U;
constexpr std::uint32_t kNonCatalystWeaponHash = 0x10000040U;

void expect(bool value, const char* label) noexcept {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

items::Definition item(std::uint16_t index,
                       std::uint32_t hash,
                       items::Tier tier = items::Tier::none,
                       std::uint32_t category = 0) noexcept {
    items::Definition value{};
    value.definitionIndex = index;
    value.definitionHash = hash;
    value.tier = static_cast<std::uint8_t>(tier);
    value.plugCategoryHash = category;
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

details::Definition plug_detail(std::uint16_t index,
                                std::uint32_t hash,
                                std::int32_t maxStackSize,
                                bool hasEffect = false) noexcept {
    details::Definition value{};
    value.definitionIndex = index;
    value.definitionHash = hash;
    value.maxStackSize = maxStackSize;
    value.instancedDefinitionState = details::InstancedDefinitionState::stackable;
    if (hasEffect) {
        value.sandboxPerkCount = 1;
        value.sandboxPerks[0] = 7;
    }
    return value;
}

struct Fixture {
    static constexpr std::uint16_t kLegacyWeapon = 10;
    static constexpr std::uint16_t kLaterWeapon = 11;
    static constexpr std::uint16_t kPlaceholderWeapon = 12;
    static constexpr std::uint16_t kNonCatalystWeapon = 13;
    static constexpr std::uint16_t kLegacyDefault = 20;
    static constexpr std::uint16_t kLegacyProgress = 21;
    static constexpr std::uint16_t kLegacyComplete = 22;
    static constexpr std::uint16_t kLegacyEffect = 23;
    static constexpr std::uint16_t kLaterDefault = 30;
    static constexpr std::uint16_t kLaterActive = 31;
    static constexpr std::uint16_t kPlaceholderDefault = 40;
    static constexpr std::uint16_t kPlaceholderActive = 41;
    static constexpr std::uint16_t kOtherDefault = 50;
    static constexpr std::uint16_t kOtherActive = 51;

    std::array<items::Definition, 13> itemRows{};
    std::array<details::Definition, 11> detailRows{};
    std::array<socket_plugs::Rule, 4> rules{{
        {kLegacyWeapon, 7, 0, 1},
        {kLaterWeapon, 6, 0, 2},
        {kPlaceholderWeapon, 5, 0, 3},
        {kNonCatalystWeapon, 4, 0, 4},
    }};
    std::array<socket_plugs::Pool, 5> pools{{
        {0, 0},
        {0, 3},
        {3, 2},
        {5, 2},
        {7, 2},
    }};
    std::array<socket_plugs::Member, 9> members{{
        kLegacyDefault,
        kLegacyProgress,
        kLegacyComplete,
        kLaterDefault,
        kLaterActive,
        kPlaceholderDefault,
        kPlaceholderActive,
        kOtherDefault,
        kOtherActive,
    }};
    std::array<std::uint32_t, 2> released{{kLegacyWeaponHash, kLaterWeaponHash}};
    std::array<catalysts::Definition, catalysts::kDefinitionCapacity> output{};

    Fixture() noexcept {
        constexpr std::uint32_t kLegacyCategory = 0x20000010U;
        constexpr std::uint32_t kLaterCategory = 0x20000020U;
        constexpr std::uint32_t kPlaceholderCategory = 0x20000030U;
        constexpr std::uint32_t kOtherCategory = 0x20000040U;
        itemRows = {
            item(kLegacyWeapon, kLegacyWeaponHash, items::Tier::exotic),
            item(kLaterWeapon, kLaterWeaponHash, items::Tier::exotic),
            item(kPlaceholderWeapon, kPlaceholderWeaponHash, items::Tier::exotic),
            item(kNonCatalystWeapon, kNonCatalystWeaponHash, items::Tier::exotic),
            item(kLegacyDefault, 0x30000010U, items::Tier::none, kLegacyCategory),
            item(kLegacyProgress, 0x30000011U, items::Tier::none, kLegacyCategory),
            item(kLegacyComplete, 0x30000012U, items::Tier::none, kLegacyCategory),
            item(kLegacyEffect, 0x30000013U, items::Tier::exotic, kLegacyCategory),
            item(kLaterDefault, 0x30000020U, items::Tier::none, kLaterCategory),
            item(kLaterActive, 0x30000021U, items::Tier::exotic, kLaterCategory),
            item(kPlaceholderDefault, 0x30000030U, items::Tier::none, kPlaceholderCategory),
            item(kPlaceholderActive, 0x30000031U, items::Tier::exotic, kPlaceholderCategory),
            item(kOtherActive, 0x30000041U, items::Tier::legendary, kOtherCategory),
        };
        detailRows = {
            weapon_detail(kLegacyWeapon, kLegacyWeaponHash, 7, kLegacyDefault),
            weapon_detail(kLaterWeapon, kLaterWeaponHash, 6, kLaterDefault),
            weapon_detail(kPlaceholderWeapon, kPlaceholderWeaponHash, 5, kPlaceholderDefault),
            weapon_detail(kNonCatalystWeapon, kNonCatalystWeaponHash, 4, kOtherDefault),
            plug_detail(kLegacyDefault, 0x30000010U, 100),
            plug_detail(kLegacyProgress, 0x30000011U, 1),
            plug_detail(kLegacyComplete, 0x30000012U, 100),
            plug_detail(kLegacyEffect, 0x30000013U, 100, true),
            plug_detail(kLaterActive, 0x30000021U, 100, true),
            plug_detail(kPlaceholderActive, 0x30000031U, 100, true),
            plug_detail(kOtherActive, 0x30000041U, 100, true),
        };
    }

    [[nodiscard]] catalysts::Source source() const noexcept {
        return {{kTimestamp, kImageSize, 0}, itemRows, detailRows, rules, pools, members};
    }

    [[nodiscard]] catalysts::Facts facts() const noexcept {
        return {kTimestamp, kImageSize, released};
    }
};

bool derive(Fixture& fixture, std::size_t& count, catalysts::Report& report) noexcept {
    return catalysts::derive(fixture.source(), fixture.facts(), fixture.output, count, report);
}

void test_structural_lifecycles() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    expect(derive(fixture, count, report), "synthetic catalyst catalog derives");
    expect(count == 3, "only two released and one placeholder catalyst are found");
    expect(report.released == 2 && report.placeholder == 1 && report.unsupported == 0,
           "catalog report separates released and placeholder rows");
    if (count != 3) {
        return;
    }
    expect(fixture.output[0].itemDefinitionIndex == Fixture::kLegacyWeapon
               && fixture.output[0].completedPlugDefinitionIndex == Fixture::kLegacyComplete
               && fixture.output[0].effectDefinitionIndex == Fixture::kLegacyEffect,
           "legacy three-state lifecycle resolves display and effect rows");
    expect(fixture.output[1].itemDefinitionIndex == Fixture::kLaterWeapon
               && fixture.output[1].completedPlugDefinitionIndex == Fixture::kLaterActive
               && fixture.output[1].effectDefinitionIndex == Fixture::kLaterActive,
           "later two-state lifecycle resolves its direct active plug");
    expect(fixture.output[2].availability == catalysts::Availability::placeholder,
           "unreleased structural catalyst remains a placeholder");
    expect(std::none_of(std::span(fixture.output).first(count).begin(),
                        std::span(fixture.output).first(count).end(),
                        [](const catalysts::Definition& value) {
                            return value.itemDefinitionIndex == Fixture::kNonCatalystWeapon;
                        }),
           "a two-state lane without an exotic effect item is not a catalyst");
    expect(catalysts::matches_derived(
               fixture.source(), fixture.facts(), std::span(fixture.output).first(count)),
           "stored catalog matches a fresh structural derivation");

    auto altered = fixture.output;
    altered[0].completedPlugDefinitionIndex = Fixture::kLegacyEffect;
    expect(!catalysts::matches_derived(
               fixture.source(), fixture.facts(), std::span(altered).first(count)),
           "catalog rejects a completed plug outside its socket pool");

    altered = fixture.output;
    altered[0].completedPlugDefinitionIndex = Fixture::kLegacyProgress;
    expect(!catalysts::matches_derived(
               fixture.source(), fixture.facts(), std::span(altered).first(count)),
           "catalog rejects a completed plug outside the derived role");

    Fixture legendaryWeapon;
    legendaryWeapon.itemRows[2].tier = static_cast<std::uint8_t>(items::Tier::legendary);
    expect(derive(legendaryWeapon, count, report) && count == 2 && report.placeholder == 0,
           "non-exotic weapons are outside catalyst scope");

    Fixture exoticArmor;
    exoticArmor.detailRows[2].equipmentSlot = std::int8_t{3};
    expect(derive(exoticArmor, count, report) && count == 2 && report.placeholder == 0,
           "exotic armor is outside catalyst scope");
}

void test_safe_derivation_failures() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    catalysts::Source mismatched = fixture.source();
    ++mismatched.build.imageSize;
    expect(!catalysts::derive(mismatched, fixture.facts(), fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::unsupportedBuild,
           "build fingerprint mismatch fails only the catalog derivation");

    std::array<std::uint32_t, 3> missingRelease{kLegacyWeaponHash, kLaterWeaponHash, 0x1FFFFFFFU};
    catalysts::Facts missingFacts{kTimestamp, kImageSize, missingRelease};
    expect(!catalysts::derive(fixture.source(), missingFacts, fixture.output, count, report)
               && count == 0 && report.error == catalysts::Error::missingReleased,
           "missing released weapon clears all staged catalog rows");

    fixture.detailRows[5].maxStackSize = 100;
    expect(!derive(fixture, count, report) && count == 0
               && report.error == catalysts::Error::missingReleased,
           "unclear released legacy lifecycle fails closed");
}

void test_catalog_application() noexcept {
    Fixture fixture;
    std::size_t count = 0;
    catalysts::Report report{};
    if (!derive(fixture, count, report)
        || !catalysts::replace(std::span(fixture.output).first(count))) {
        expect(false, "application fixture publishes");
        return;
    }

    for (std::uint32_t flags = 0; flags <= 3; ++flags) {
        std::array<std::optional<std::uint16_t>, 12> plugs{};
        plugs[7] = Fixture::kLegacyDefault;
        std::uint32_t changedFlags = flags;
        expect(catalysts::apply_completed(Fixture::kLegacyWeapon, changedFlags, plugs)
                   == catalysts::ApplyResult::completed,
               "released legacy catalyst completes");
        expect(changedFlags == (flags | sunrise::state::account::inventory::kMasterworkItemFlag),
               "completion preserves all prior item-state bits");
        expect(plugs[7] == Fixture::kLegacyComplete,
               "completion sockets the display plug from the allowed pool");
    }
    expect(catalysts::resolve_effect(Fixture::kLegacyWeapon, 7, Fixture::kLegacyComplete)
               == Fixture::kLegacyEffect,
           "legacy display plug resolves its effect item");
    expect(catalysts::resolve_effect(Fixture::kLegacyWeapon, 7, Fixture::kLegacyDefault)
               == Fixture::kLegacyDefault,
           "uncompleted display plug remains unchanged");

    std::array<std::optional<std::uint16_t>, 12> laterPlugs{};
    laterPlugs[6] = Fixture::kLaterDefault;
    std::uint32_t laterFlags = 0;
    expect(catalysts::apply_completed(Fixture::kLaterWeapon, laterFlags, laterPlugs)
                   == catalysts::ApplyResult::completed
               && laterPlugs[6] == Fixture::kLaterActive,
           "later catalyst sockets its direct active plug");

    std::array<std::optional<std::uint16_t>, 12> placeholderPlugs{};
    placeholderPlugs[5] = Fixture::kPlaceholderDefault;
    const auto placeholderBefore = placeholderPlugs;
    std::uint32_t placeholderFlags = 3;
    expect(
        catalysts::apply_completed(Fixture::kPlaceholderWeapon, placeholderFlags, placeholderPlugs)
                == catalysts::ApplyResult::unchanged
            && placeholderFlags == 3 && placeholderPlugs == placeholderBefore,
        "placeholder catalyst remains unchanged");

    std::array<std::optional<std::uint16_t>, 12> invalidPlugs{};
    invalidPlugs[7] = Fixture::kLegacyDefault;
    const auto invalidBefore = invalidPlugs;
    std::uint32_t invalidFlags = 0x8U;
    expect(catalysts::apply_completed(Fixture::kLegacyWeapon, invalidFlags, invalidPlugs)
                   == catalysts::ApplyResult::failed
               && invalidFlags == 0x8U && invalidPlugs == invalidBefore,
           "invalid item state fails without a partial change");

    std::array<std::optional<std::uint16_t>, 7> shortPlugs{};
    const auto shortBefore = shortPlugs;
    std::uint32_t shortFlags = 1;
    expect(catalysts::apply_completed(Fixture::kLegacyWeapon, shortFlags, shortPlugs)
                   == catalysts::ApplyResult::failed
               && shortFlags == 1 && shortPlugs == shortBefore,
           "missing catalyst lane fails without a partial change");

    catalysts::set_completion_enabled(false);
    std::array<std::optional<std::uint16_t>, 12> disabledPlugs{};
    disabledPlugs[7] = Fixture::kLegacyDefault;
    const auto disabledBefore = disabledPlugs;
    std::uint32_t disabledFlags = 2;
    expect(catalysts::apply_completed(Fixture::kLegacyWeapon, disabledFlags, disabledPlugs)
                   == catalysts::ApplyResult::unchanged
               && disabledFlags == 2 && disabledPlugs == disabledBefore,
           "global policy disables completion without changing the item");
    catalysts::clear();
}

void test_cache_record() noexcept {
    const catalysts::Definition definition{
        0x11111111U, 3, 7, 9, 5, catalysts::Availability::released};
    cache_records::ExoticCatalystRecord record{};
    catalysts::Definition decoded{};
    expect(cache_records::encode(definition, record), "catalyst cache record encodes");
    expect(cache_records::decode(record, decoded)
               && decoded.itemDefinitionHash == definition.itemDefinitionHash
               && decoded.itemDefinitionIndex == definition.itemDefinitionIndex
               && decoded.completedPlugDefinitionIndex == definition.completedPlugDefinitionIndex
               && decoded.effectDefinitionIndex == definition.effectDefinitionIndex
               && decoded.socketLane == definition.socketLane
               && decoded.availability == definition.availability,
           "catalyst cache record round-trips");
    record.availability = 0xFFU;
    expect(!cache_records::decode(record, decoded), "invalid catalyst availability is rejected");
    expect(cache_records::kCacheFormatVersion == 45,
           "catalyst records use one cache bump over upstream version 44");
}

void test_persistence_action() noexcept {
    using enum persistence::CacheAction;
    expect(persistence::cache_action(false, false, catalysts::Error::unsupportedBuild)
               == waitForDomains,
           "persistence waits for required domains");
    expect(persistence::cache_action(true, false, catalysts::Error::unsupportedBuild)
               == skipUnsupportedCatalog,
           "unsupported builds finish without an incomplete cache");
    constexpr std::array rejectedErrors{
        catalysts::Error::none,
        catalysts::Error::noCatalyst,
        catalysts::Error::placeholderOnly,
        catalysts::Error::missingReleased,
        catalysts::Error::ambiguousLifecycle,
        catalysts::Error::invalidSocket,
    };
    for (const catalysts::Error error : rejectedErrors) {
        expect(persistence::cache_action(true, false, error) == waitForDomains,
               "non-build catalyst failures stay failed closed");
    }
    expect(persistence::cache_action(true, true, catalysts::Error::none) == writeCompleteCache,
           "a complete catalog permits one cache write");
}

} // namespace

void test_exotic_catalysts() noexcept {
    test_structural_lifecycles();
    test_safe_derivation_failures();
    test_catalog_application();
    test_cache_record();
    test_persistence_action();
    catalysts::clear();
}
