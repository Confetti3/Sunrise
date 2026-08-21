#include "exotic_catalyst_builder.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>

namespace sunrise::state::build_data::items::catalysts {
namespace {

/** Native equipment slots 7, 8, and 9 are kinetic, energy, and heavy weapons. */
constexpr std::int8_t kFirstWeaponSlot = 7;
constexpr std::int8_t kLastWeaponSlot = 9;

struct LaneResult {
    bool catalyst{};
    Error error{Error::none};
    std::uint16_t completedPlugDefinitionIndex{details::kUnavailableItemIndex};
    std::uint16_t effectDefinitionIndex{details::kUnavailableItemIndex};
};

/**
 * @param hashes Sorted hashes to validate.
 * @return True when the table is strict and contains no zero hash.
 */
[[nodiscard]] bool valid_hashes(std::span<const std::uint32_t> hashes) noexcept {
    return std::none_of(hashes.begin(), hashes.end(), [](std::uint32_t hash) { return hash == 0; })
           && std::adjacent_find(hashes.begin(), hashes.end(), std::greater_equal{})
                  == hashes.end();
}

/**
 * Finds one row in a dense or sorted native-index table.
 * @tparam Value Row type.
 * @tparam Index Returns the row's native index.
 * @param definitions Source rows in native index order.
 * @param index Native index to find.
 * @param indexOf Row-index reader.
 * @return The matching row, or null when no row has the index.
 */
template <typename Value, typename Index>
[[nodiscard]] const Value*
find_indexed(std::span<const Value> definitions, std::uint16_t index, Index indexOf) noexcept {
    if (static_cast<std::size_t>(index) < definitions.size()
        && indexOf(definitions[index]) == index) {
        return &definitions[index];
    }
    const auto found = std::lower_bound(
        definitions.begin(), definitions.end(), index, [&indexOf](const Value& value, auto key) {
            return indexOf(value) < key;
        });
    return found != definitions.end() && indexOf(*found) == index ? &*found : nullptr;
}

/**
 * @param value Installed item row.
 * @return The row's native item index.
 */
[[nodiscard]] constexpr std::uint16_t item_index(const items::Definition& value) noexcept {
    return value.definitionIndex;
}

/**
 * @param value Installed item detail row.
 * @return The detail row's native item index.
 */
[[nodiscard]] constexpr std::uint16_t detail_index(const details::Definition& value) noexcept {
    return value.definitionIndex;
}

/**
 * @param definitions Installed item rows in native index order.
 * @param index Native item index to find.
 * @return The matching source item, or null.
 */
[[nodiscard]] const items::Definition* find_item(std::span<const items::Definition> definitions,
                                                 std::uint16_t index) noexcept {
    return find_indexed(definitions, index, item_index);
}

/**
 * @param definitions Installed item details in native index order.
 * @param index Native item index to find.
 * @return The matching source detail, or null.
 */
[[nodiscard]] const details::Definition*
find_detail(std::span<const details::Definition> definitions, std::uint16_t index) noexcept {
    return find_indexed(definitions, index, detail_index);
}

/**
 * Resolves the exotic item row that owns one completed plug's native perks and stat changes.
 * Later catalysts use that item as their socket plug. Legacy sockets use a display-only plug in
 * the same category, so the unique exotic stackable item in that category supplies the effect.
 * @param source Parsed target-build tables.
 * @param completedPlugDefinitionIndex Completed display or active plug from the socket pool.
 * @return Completed lane relation, or an effect mapping error.
 */
[[nodiscard]] LaneResult complete_lane(const Source& source,
                                       std::uint16_t completedPlugDefinitionIndex) noexcept {
    const items::Definition* completed = find_item(source.items, completedPlugDefinitionIndex);
    if (completed == nullptr || completed->plugCategoryHash == 0) {
        return {};
    }

    std::optional<std::uint16_t> effect;
    for (const items::Definition& candidate : source.items) {
        if (candidate.plugCategoryHash != completed->plugCategoryHash
            || candidate.tier != static_cast<std::uint8_t>(items::Tier::exotic)) {
            continue;
        }
        const details::Definition* detail = find_detail(source.details, candidate.definitionIndex);
        if (detail == nullptr || detail->definitionHash != candidate.definitionHash
            || detail->instancedDefinitionState != details::InstancedDefinitionState::stackable
            || (detail->sandboxPerkCount == 0 && detail->statCount == 0)) {
            continue;
        }
        if (effect.has_value()) {
            return {};
        }
        effect = candidate.definitionIndex;
    }
    if (!effect.has_value()) {
        return {};
    }

    return {.catalyst = true,
            .error = Error::none,
            .completedPlugDefinitionIndex = completedPlugDefinitionIndex,
            .effectDefinitionIndex = *effect};
}

/**
 * Finds one exact item and lane rule in its canonical order.
 * @param rules Socket rules in item and lane order.
 * @param itemDefinitionIndex Native item index to find.
 * @param lane Native socket lane to find.
 * @return The matching rule, or null when no rule matches.
 */
[[nodiscard]] const socket_plugs::Rule* find_rule(std::span<const socket_plugs::Rule> rules,
                                                  std::uint16_t itemDefinitionIndex,
                                                  std::uint8_t lane) noexcept {
    const auto found = std::lower_bound(rules.begin(),
                                        rules.end(),
                                        std::pair{itemDefinitionIndex, lane},
                                        [](const socket_plugs::Rule& value, const auto& key) {
                                            return value.itemDefinitionIndex < key.first
                                                   || (value.itemDefinitionIndex == key.first
                                                       && value.lane < key.second);
                                        });
    return found != rules.end() && found->itemDefinitionIndex == itemDefinitionIndex
                   && found->lane == lane
               ? &*found
               : nullptr;
}

/**
 * @param released Sorted released weapon hashes.
 * @param hash Weapon hash to find.
 * @return Index of a released hash, or no value for a placeholder.
 */
[[nodiscard]] std::optional<std::size_t> released_index(std::span<const std::uint32_t> released,
                                                        std::uint32_t hash) noexcept {
    const auto found = std::lower_bound(released.begin(), released.end(), hash);
    if (found == released.end() || *found != hash) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - released.begin());
}

/**
 * Classifies one lane from its installed two-state or legacy three-state lifecycle.
 * @param source Parsed target-build tables.
 * @param detail Item detail row that owns the lane.
 * @param lane Native socket lane to classify.
 * @return The catalyst state and any safe-failure reason.
 */
[[nodiscard]] LaneResult
classify_lane(const Source& source, const details::Definition& detail, std::uint8_t lane) noexcept {
    const socket_plugs::Rule* rule =
        find_rule(source.socketPlugRules, detail.definitionIndex, lane);
    if (rule == nullptr) {
        return {};
    }
    if (rule->reserved != 0 || rule->poolIndex >= source.socketPlugPools.size()) {
        return {.catalyst = true, .error = Error::invalidSocket};
    }
    const socket_plugs::Pool& pool = source.socketPlugPools[rule->poolIndex];
    if (pool.memberOffset > source.socketPlugMembers.size()
        || pool.memberCount > source.socketPlugMembers.size() - pool.memberOffset) {
        return {.catalyst = true, .error = Error::invalidSocket};
    }

    const auto members = source.socketPlugMembers.subspan(pool.memberOffset, pool.memberCount);
    if (members.size() != 2 && members.size() != 3) {
        return {};
    }

    const std::uint16_t defaultIndex = detail.initialPlugIndices[lane];
    const items::Definition* defaultPlug = find_item(source.items, defaultIndex);
    if (defaultIndex == details::kUnavailableItemIndex || defaultPlug == nullptr
        || std::find(members.begin(), members.end(), defaultIndex) == members.end()) {
        return {};
    }

    if (members.size() == 2) {
        const auto active =
            std::find_if(members.begin(), members.end(), [defaultIndex](auto member) {
                return member != defaultIndex;
            });
        if (active == members.end()) {
            return {};
        }
        const LaneResult completed = complete_lane(source, *active);
        return completed.catalyst && completed.effectDefinitionIndex == *active ? completed
                                                                                : LaneResult{};
    }

    std::size_t progressCount = 0;
    std::size_t completedCount = 0;
    std::uint16_t completedIndex = details::kUnavailableItemIndex;
    for (const std::uint16_t member : members) {
        if (member == defaultIndex) {
            continue;
        }
        const items::Definition* plug = find_item(source.items, member);
        const details::Definition* plugDetail = find_detail(source.details, member);
        if (plug == nullptr || plugDetail == nullptr
            || plugDetail->definitionHash != plug->definitionHash
            || plugDetail->instancedDefinitionState
                   != details::InstancedDefinitionState::stackable) {
            return {};
        }
        if (plugDetail->maxStackSize == 1) {
            ++progressCount;
        } else if (plugDetail->maxStackSize > 1) {
            ++completedCount;
            completedIndex = member;
        }
    }
    if (progressCount == 1 && completedCount == 1) {
        const LaneResult completed = complete_lane(source, completedIndex);
        return completed.catalyst && completed.effectDefinitionIndex != completedIndex
                   ? completed
                   : LaneResult{};
    }
    return {};
}

/**
 * Clears staged output and records one safe failure.
 * @param output Staged catalog rows to clear.
 * @param count Staged row count to reset.
 * @param report Build report to update.
 * @param error Failure reason.
 * @param itemDefinitionHash Item hash that caused the failure, if known.
 * @param lane Socket lane that caused the failure, if known.
 * @return Always false.
 */
[[nodiscard]] bool fail(std::span<Definition> output,
                        std::size_t& count,
                        Report& report,
                        Error error,
                        std::uint32_t itemDefinitionHash = 0,
                        std::uint8_t lane = 0) noexcept {
    std::fill(output.begin(), output.end(), Definition{});
    count = 0;
    ++report.unsupported;
    report.error = error;
    report.itemDefinitionHash = itemDefinitionHash;
    report.socketLane = lane;
    return false;
}

/**
 * @param item Source item row.
 * @param detail Source item detail row.
 * @return True for an exotic in one of the three weapon equipment slots.
 */
[[nodiscard]] bool exotic_weapon(const items::Definition& item,
                                 const details::Definition& detail) noexcept {
    return item.tier == static_cast<std::uint8_t>(items::Tier::exotic)
           && detail.instancedDefinitionState == details::InstancedDefinitionState::instanced
           && detail.equipmentSlot.has_value() && *detail.equipmentSlot >= kFirstWeaponSlot
           && *detail.equipmentSlot <= kLastWeaponSlot
           && detail.ordinarySocketState == details::OrdinarySocketState::present
           && detail.ordinarySocketCount <= details::kInitialPlugCapacity;
}

} // namespace

bool derive(const Source& source,
            const Facts& facts,
            std::span<Definition> output,
            std::size_t& count,
            Report& report) noexcept {
    count = 0;
    report = {};
    std::fill(output.begin(), output.end(), Definition{});
    if (facts.imageTimestamp == 0 || facts.imageSize == 0
        || facts.releasedWeaponHashes.size() > kDefinitionCapacity
        || !valid_hashes(facts.releasedWeaponHashes)) {
        return fail(output, count, report, Error::unsupportedBuild);
    }
    if (source.build.imageTimestamp != facts.imageTimestamp
        || source.build.imageSize != facts.imageSize) {
        return fail(output, count, report, Error::unsupportedBuild);
    }

    std::array<bool, kDefinitionCapacity> releasedFound{};
    for (const details::Definition& detail : source.details) {
        const items::Definition* item = find_item(source.items, detail.definitionIndex);
        if (item == nullptr || item->definitionHash != detail.definitionHash
            || !exotic_weapon(*item, detail)) {
            continue;
        }

        std::optional<std::size_t> release =
            released_index(facts.releasedWeaponHashes, item->definitionHash);
        std::optional<CompletedCatalyst> completed;
        std::optional<Error> unclear;
        std::uint8_t detectedLane = 0;
        for (std::size_t laneIndex = 0; laneIndex < detail.ordinarySocketCount; ++laneIndex) {
            const auto lane = static_cast<std::uint8_t>(laneIndex);
            const LaneResult result = classify_lane(source, detail, lane);
            if (!result.catalyst) {
                continue;
            }
            detectedLane = lane;
            if (result.error != Error::none) {
                unclear = result.error;
                continue;
            }
            if (completed.has_value()) {
                unclear = Error::ambiguousLifecycle;
                continue;
            }
            completed = CompletedCatalyst{
                lane, result.completedPlugDefinitionIndex, result.effectDefinitionIndex};
        }
        if (!completed.has_value() && !unclear.has_value()) {
            continue;
        }
        if (unclear.has_value()) {
            if (release.has_value()) {
                return fail(output, count, report, *unclear, item->definitionHash, detectedLane);
            }
            if (count >= output.size() || count >= kDefinitionCapacity) {
                return fail(output,
                            count,
                            report,
                            Error::invalidSocket,
                            item->definitionHash,
                            detectedLane);
            }
            output[count++] = Definition{item->definitionHash,
                                         item->definitionIndex,
                                         details::kUnavailableItemIndex,
                                         details::kUnavailableItemIndex,
                                         detectedLane,
                                         Availability::unsupported};
            ++report.unsupported;
            continue;
        }
        if (count >= output.size() || count >= kDefinitionCapacity) {
            return fail(output,
                        count,
                        report,
                        Error::invalidSocket,
                        item->definitionHash,
                        completed->socketLane);
        }
        if (release.has_value()) {
            if (releasedFound[*release]) {
                return fail(output,
                            count,
                            report,
                            Error::ambiguousLifecycle,
                            item->definitionHash,
                            completed->socketLane);
            }
            releasedFound[*release] = true;
            ++report.released;
        } else {
            ++report.placeholder;
        }
        output[count++] =
            Definition{item->definitionHash,
                       item->definitionIndex,
                       completed->completedPlugDefinitionIndex,
                       completed->effectDefinitionIndex,
                       completed->socketLane,
                       release.has_value() ? Availability::released : Availability::placeholder};
    }

    for (std::size_t index = 0; index < facts.releasedWeaponHashes.size(); ++index) {
        if (!releasedFound[index]) {
            return fail(
                output, count, report, Error::missingReleased, facts.releasedWeaponHashes[index]);
        }
    }
    std::sort(output.begin(),
              output.begin() + count,
              [](const Definition& left, const Definition& right) {
                  return left.itemDefinitionIndex < right.itemDefinitionIndex;
              });
    return true;
}

bool matches_derived(const Source& source,
                     const Facts& facts,
                     std::span<const Definition> definitions) noexcept {
    std::array<Definition, kDefinitionCapacity> expected{};
    std::size_t expectedCount = 0;
    Report report{};
    if (!derive(source, facts, expected, expectedCount, report)
        || expectedCount != definitions.size()) {
        return false;
    }
    return std::equal(expected.begin(),
                      expected.begin() + expectedCount,
                      definitions.begin(),
                      [](const Definition& left, const Definition& right) {
                          return left.itemDefinitionHash == right.itemDefinitionHash
                                 && left.itemDefinitionIndex == right.itemDefinitionIndex
                                 && left.completedPlugDefinitionIndex
                                        == right.completedPlugDefinitionIndex
                                 && left.effectDefinitionIndex == right.effectDefinitionIndex
                                 && left.socketLane == right.socketLane
                                 && left.availability == right.availability;
                      });
}

} // namespace sunrise::state::build_data::items::catalysts
