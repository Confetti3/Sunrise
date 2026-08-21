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
    std::uint16_t acquisitionDefinitionIndex{kUnavailableAcquisitionIndex};
    std::uint16_t completionFlagDefinitionIndex{kUnavailableCompletionFlagIndex};
    std::uint16_t completionValueIndex{kUnavailableCompletionValueIndex};
    std::int32_t completionValue{};
};

/**
 * @param hashes Sorted hashes to validate.
 * @return True when the table is strict and contains no zero hash.
 */
[[nodiscard]] bool valid_hashes(std::span<const std::uint32_t> hashes) noexcept {
    return std::none_of(
               hashes.begin(), hashes.end(), [](std::uint32_t hash) { return hash == 0; })
           && std::adjacent_find(hashes.begin(), hashes.end(), std::greater_equal{})
                  == hashes.end();
}

/**
 * Finds a source item without requiring a dense test fixture.
 * @param definitions Source item definitions in native index order.
 * @param index Native item index to find.
 * @return The matching item, or null when no item has the index.
 */
[[nodiscard]] const items::Definition*
find_item(std::span<const items::Definition> definitions, std::uint16_t index) noexcept {
    if (static_cast<std::size_t>(index) < definitions.size()
        && definitions[index].definitionIndex == index) {
        return &definitions[index];
    }
    const auto found = std::lower_bound(
        definitions.begin(),
        definitions.end(),
        index,
        [](const items::Definition& value, auto key) { return value.definitionIndex < key; });
    return found != definitions.end() && found->definitionIndex == index ? &*found : nullptr;
}

/**
 * Finds a source detail without requiring a dense test fixture.
 * @param definitions Source item details in native index order.
 * @param index Native item index to find.
 * @return The matching detail, or null when no detail has the index.
 */
[[nodiscard]] const details::Definition*
find_detail(std::span<const details::Definition> definitions, std::uint16_t index) noexcept {
    if (static_cast<std::size_t>(index) < definitions.size()
        && definitions[index].definitionIndex == index) {
        return &definitions[index];
    }
    const auto found = std::lower_bound(
        definitions.begin(),
        definitions.end(),
        index,
        [](const details::Definition& value, auto key) { return value.definitionIndex < key; });
    return found != definitions.end() && found->definitionIndex == index ? &*found : nullptr;
}

/** Finds one dense or sorted completion-condition row. */
[[nodiscard]] const CompletionCondition*
find_completion_condition(std::span<const CompletionCondition> definitions,
                          std::uint16_t index) noexcept {
    if (static_cast<std::size_t>(index) < definitions.size()
        && definitions[index].itemDefinitionIndex == index) {
        return &definitions[index];
    }
    const auto found = std::lower_bound(
        definitions.begin(),
        definitions.end(),
        index,
        [](const CompletionCondition& value, auto key) {
            return value.itemDefinitionIndex < key;
        });
    return found != definitions.end() && found->itemDefinitionIndex == index ? &*found : nullptr;
}

/** Finds one dense or sorted socket-type acquisition row. */
[[nodiscard]] const AcquisitionGate*
find_acquisition_gate(std::span<const AcquisitionGate> definitions,
                      std::uint16_t socketType) noexcept {
    if (static_cast<std::size_t>(socketType) < definitions.size()
        && definitions[socketType].socketType == socketType) {
        return &definitions[socketType];
    }
    const auto found = std::lower_bound(
        definitions.begin(),
        definitions.end(),
        socketType,
        [](const AcquisitionGate& value, auto key) { return value.socketType < key; });
    return found != definitions.end() && found->socketType == socketType ? &*found : nullptr;
}

/**
 * Resolves the exotic item row that owns one completed plug's native perks and stat changes.
 * Later catalysts use that item as their socket plug. Legacy sockets use a display-only plug in
 * the same category, so the unique exotic stackable item in that category supplies the effect.
 * @param source Parsed target-build tables.
 * @param completedPlugDefinitionIndex Completed display or active plug from the socket pool.
 * @param socketType Native socket type that owns the acquired-state gate.
 * @return Completed lane relation, or an effect mapping error.
 */
[[nodiscard]] LaneResult complete_lane(const Source& source,
                                       std::uint16_t completedPlugDefinitionIndex,
                                       std::uint16_t socketType) noexcept {
    const items::Definition* completed = find_item(source.items, completedPlugDefinitionIndex);
    if (completed == nullptr || completed->plugCategoryHash == 0) {
        return {.catalyst = true,
                .error = Error::invalidEffect,
                .completedPlugDefinitionIndex = completedPlugDefinitionIndex};
    }

    const AcquisitionGate* acquisition =
        find_acquisition_gate(source.acquisitionGates, socketType);
    if (acquisition == nullptr || acquisition->state != AcquisitionState::present
        || acquisition->definitionIndex == kUnavailableAcquisitionIndex) {
        return {.catalyst = true,
                .error = Error::invalidAcquisition,
                .completedPlugDefinitionIndex = completedPlugDefinitionIndex};
    }

    std::optional<std::uint16_t> effect;
    for (const items::Definition& candidate : source.items) {
        if (candidate.plugCategoryHash != completed->plugCategoryHash
            || candidate.tier != static_cast<std::uint8_t>(items::Tier::exotic)) {
            continue;
        }
        const details::Definition* detail = find_detail(source.details, candidate.definitionIndex);
        if (detail == nullptr || detail->definitionHash != candidate.definitionHash
            || detail->instancedDefinitionState
                   != details::InstancedDefinitionState::stackable
            || (detail->sandboxPerkCount == 0 && detail->statCount == 0)) {
            continue;
        }
        if (effect.has_value()) {
            return {.catalyst = true,
                    .error = Error::invalidEffect,
                    .completedPlugDefinitionIndex = completedPlugDefinitionIndex};
        }
        effect = candidate.definitionIndex;
    }
    if (!effect.has_value()) {
        return {.catalyst = true,
                .error = Error::invalidEffect,
                .completedPlugDefinitionIndex = completedPlugDefinitionIndex};
    }

    const CompletionCondition* condition =
        find_completion_condition(source.completionConditions, *effect);
    const bool hasFlag = condition != nullptr
                         && condition->flagDefinitionIndex
                                != kUnavailableCompletionFlagIndex;
    const bool hasValue = condition != nullptr
                          && condition->valueIndex != kUnavailableCompletionValueIndex
                          && condition->value > 0;
    const bool directEffect = *effect == completedPlugDefinitionIndex;
    if (condition == nullptr || condition->state != CompletionConditionState::present
        || (directEffect && !hasFlag) || (!directEffect && !hasValue)) {
        return {.catalyst = true,
                .error = Error::invalidCompletion,
                .completedPlugDefinitionIndex = completedPlugDefinitionIndex,
                .effectDefinitionIndex = *effect,
                .acquisitionDefinitionIndex = acquisition->definitionIndex};
    }
    return {.catalyst = true,
            .error = Error::none,
            .completedPlugDefinitionIndex = completedPlugDefinitionIndex,
            .effectDefinitionIndex = *effect,
            .acquisitionDefinitionIndex = acquisition->definitionIndex,
            .completionFlagDefinitionIndex = condition->flagDefinitionIndex,
            .completionValueIndex = condition->valueIndex,
            .completionValue = condition->value};
}

/**
 * Finds one exact item and lane rule in its canonical order.
 * @param rules Socket rules in item and lane order.
 * @param itemDefinitionIndex Native item index to find.
 * @param lane Native socket lane to find.
 * @return The matching rule, or null when no rule matches.
 */
[[nodiscard]] const socket_plugs::Rule*
find_rule(std::span<const socket_plugs::Rule> rules,
          std::uint16_t itemDefinitionIndex,
          std::uint8_t lane) noexcept {
    const auto found = std::lower_bound(
        rules.begin(),
        rules.end(),
        std::pair{itemDefinitionIndex, lane},
        [](const socket_plugs::Rule& value, const auto& key) {
            return value.itemDefinitionIndex < key.first
                   || (value.itemDefinitionIndex == key.first && value.lane < key.second);
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
[[nodiscard]] std::optional<std::size_t>
released_index(std::span<const std::uint32_t> released, std::uint32_t hash) noexcept {
    const auto found = std::lower_bound(released.begin(), released.end(), hash);
    if (found == released.end() || *found != hash) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - released.begin());
}

/**
 * Classifies one lane when its pool has a target-build catalyst marker.
 * @param source Parsed target-build tables.
 * @param facts Pinned facts for the target build.
 * @param detail Item detail row that owns the lane.
 * @param lane Native socket lane to classify.
 * @return The catalyst state and any safe-failure reason.
 */
[[nodiscard]] LaneResult classify_lane(const Source& source,
                                       const Facts& facts,
                                       const details::Definition& detail,
                                       std::uint8_t lane) noexcept {
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
    bool hasEmpty = false;
    std::size_t legacyCount = 0;
    std::uint16_t legacyCompletionIndex = 0;
    bool invalidMember = false;
    for (const std::uint16_t member : members) {
        const items::Definition* plug = find_item(source.items, member);
        if (plug == nullptr) {
            invalidMember = true;
            continue;
        }
        if (plug->definitionHash == facts.emptyCatalystPlugHash) {
            hasEmpty = true;
        }
        if (std::binary_search(facts.legacyCompletionPlugHashes.begin(),
                               facts.legacyCompletionPlugHashes.end(),
                               plug->definitionHash)) {
            ++legacyCount;
            legacyCompletionIndex = member;
        }
    }
    if (!hasEmpty && legacyCount == 0) {
        return {};
    }
    if (invalidMember) {
        return {.catalyst = true, .error = Error::invalidPlug};
    }

    const std::uint16_t defaultIndex = detail.initialPlugIndices[lane];
    const items::Definition* defaultPlug = find_item(source.items, defaultIndex);
    if (defaultIndex == details::kUnavailableItemIndex || defaultPlug == nullptr
        || std::find(members.begin(), members.end(), defaultIndex) == members.end()) {
        return {.catalyst = true, .error = Error::invalidPlug};
    }

    if (members.size() == 2 && hasEmpty && legacyCount == 0
        && defaultPlug->definitionHash == facts.emptyCatalystPlugHash) {
        const auto active =
            std::find_if(members.begin(), members.end(), [defaultIndex](auto member) {
                return member != defaultIndex;
            });
        return active != members.end()
                   ? complete_lane(source, *active, detail.socketTypes[lane])
                   : LaneResult{.catalyst = true, .error = Error::ambiguousLifecycle};
    }
    if (members.size() == 3 && !hasEmpty && legacyCount == 1
        && legacyCompletionIndex != defaultIndex) {
        return complete_lane(source, legacyCompletionIndex, detail.socketTypes[lane]);
    }
    return {.catalyst = true, .error = Error::ambiguousLifecycle};
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
    if (facts.imageTimestamp == 0 || facts.imageSize == 0 || facts.emptyCatalystPlugHash == 0
        || facts.legacyCompletionPlugHashes.size() > kDefinitionCapacity
        || facts.releasedWeaponHashes.size() > kDefinitionCapacity
        || !valid_hashes(facts.legacyCompletionPlugHashes)
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
            const LaneResult result = classify_lane(source, facts, detail, lane);
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
            completed = CompletedCatalyst{lane,
                                          result.completedPlugDefinitionIndex,
                                          result.effectDefinitionIndex,
                                          result.acquisitionDefinitionIndex,
                                          result.completionFlagDefinitionIndex,
                                          result.completionValueIndex,
                                          result.completionValue};
        }
        if (!completed.has_value() && !unclear.has_value()) {
            continue;
        }
        if (unclear.has_value()) {
            if (release.has_value()) {
                return fail(output,
                            count,
                            report,
                            *unclear,
                            item->definitionHash,
                            detectedLane);
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
                                         kUnavailableAcquisitionIndex,
                                         kUnavailableCompletionFlagIndex,
                                         kUnavailableCompletionValueIndex,
                                         detectedLane,
                                         Availability::unsupported,
                                         0};
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
        output[count++] = Definition{item->definitionHash,
                                     item->definitionIndex,
                                     completed->completedPlugDefinitionIndex,
                                     completed->effectDefinitionIndex,
                                     completed->acquisitionDefinitionIndex,
                                     completed->completionFlagDefinitionIndex,
                                     completed->completionValueIndex,
                                     completed->socketLane,
                                     release.has_value() ? Availability::released
                                                         : Availability::placeholder,
                                     completed->completionValue};
    }

    for (std::size_t index = 0; index < facts.releasedWeaponHashes.size(); ++index) {
        if (!releasedFound[index]) {
            return fail(output,
                        count,
                        report,
                        Error::missingReleased,
                        facts.releasedWeaponHashes[index]);
        }
    }
    std::sort(output.begin(), output.begin() + count, [](const Definition& left,
                                                         const Definition& right) {
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
    return std::equal(
        expected.begin(),
        expected.begin() + expectedCount,
        definitions.begin(),
        [](const Definition& left, const Definition& right) {
            return left.itemDefinitionHash == right.itemDefinitionHash
                   && left.itemDefinitionIndex == right.itemDefinitionIndex
                   && left.completedPlugDefinitionIndex == right.completedPlugDefinitionIndex
                   && left.effectDefinitionIndex == right.effectDefinitionIndex
                   && left.acquisitionDefinitionIndex == right.acquisitionDefinitionIndex
                   && left.completionFlagDefinitionIndex
                          == right.completionFlagDefinitionIndex
                   && left.completionValueIndex == right.completionValueIndex
                   && left.socketLane == right.socketLane
                   && left.availability == right.availability
                   && left.completionValue == right.completionValue;
        });
}

bool matches_cached(const Source& source,
                    const Facts& facts,
                    std::span<const Definition> definitions) noexcept {
    std::array<CompletionCondition, kDefinitionCapacity> completionConditions{};
    std::array<AcquisitionGate, kDefinitionCapacity> acquisitionGates{};
    std::size_t completionCount = 0;
    std::size_t acquisitionCount = 0;
    for (const Definition& definition : definitions) {
        if (definition.availability == Availability::unsupported) {
            continue;
        }
        const details::Definition* detail = find_detail(source.details,
                                                        definition.itemDefinitionIndex);
        if (detail == nullptr || definition.socketLane >= detail->ordinarySocketCount
            || find_item(source.items, definition.acquisitionDefinitionIndex) == nullptr
            || (definition.completionFlagDefinitionIndex
                        != kUnavailableCompletionFlagIndex
                && find_item(source.items, definition.completionFlagDefinitionIndex)
                       == nullptr)) {
            return false;
        }

        const std::uint16_t socketType = detail->socketTypes[definition.socketLane];
        const auto priorGate = std::find_if(
            acquisitionGates.begin(),
            acquisitionGates.begin() + acquisitionCount,
            [socketType](const AcquisitionGate& gate) { return gate.socketType == socketType; });
        if (priorGate != acquisitionGates.begin() + acquisitionCount) {
            if (priorGate->definitionIndex != definition.acquisitionDefinitionIndex) {
                return false;
            }
        } else if (acquisitionCount >= acquisitionGates.size()) {
            return false;
        } else {
            acquisitionGates[acquisitionCount++] = {
                socketType, definition.acquisitionDefinitionIndex, AcquisitionState::present};
        }

        if (definition.completionFlagDefinitionIndex == kUnavailableCompletionFlagIndex
            && definition.completionValueIndex == kUnavailableCompletionValueIndex) {
            continue;
        }
        const auto priorCondition = std::find_if(
            completionConditions.begin(),
            completionConditions.begin() + completionCount,
            [&definition](const CompletionCondition& condition) {
                return condition.itemDefinitionIndex == definition.effectDefinitionIndex;
            });
        if (priorCondition != completionConditions.begin() + completionCount) {
            if (priorCondition->flagDefinitionIndex
                    != definition.completionFlagDefinitionIndex
                || priorCondition->valueIndex != definition.completionValueIndex
                || priorCondition->value != definition.completionValue) {
                return false;
            }
        } else if (completionCount >= completionConditions.size()) {
            return false;
        } else {
            completionConditions[completionCount++] = {
                definition.effectDefinitionIndex,
                definition.completionFlagDefinitionIndex,
                definition.completionValueIndex,
                definition.completionValue,
                CompletionConditionState::present};
        }
    }
    std::sort(completionConditions.begin(),
              completionConditions.begin() + completionCount,
              [](const CompletionCondition& left, const CompletionCondition& right) {
                  return left.itemDefinitionIndex < right.itemDefinitionIndex;
              });
    std::sort(acquisitionGates.begin(),
              acquisitionGates.begin() + acquisitionCount,
              [](const AcquisitionGate& left, const AcquisitionGate& right) {
                  return left.socketType < right.socketType;
              });
    Source rebuilt = source;
    rebuilt.completionConditions = std::span(completionConditions).first(completionCount);
    rebuilt.acquisitionGates = std::span(acquisitionGates).first(acquisitionCount);
    return matches_derived(rebuilt, facts, definitions);
}

} // namespace sunrise::state::build_data::items::catalysts
