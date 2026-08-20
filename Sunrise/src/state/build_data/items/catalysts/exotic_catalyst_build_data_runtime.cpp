#include "../../runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>

#include "../../runtime/persistence/build_data_persistence.h"
#include "../../runtime/persistence/publication_transaction.h"
#include "../details/item_detail_catalog.h"
#include "../item_catalog.h"
#include "../socket_plugs/socket_plug_catalog.h"
#include "exotic_catalyst_builder.h"
#include "exotic_catalyst_catalog.h"

namespace sunrise::state::build_data {
namespace {

/** Checks one derived record against all published source domains. */
[[nodiscard]] bool valid_publication(
    std::span<const items::catalysts::Definition> definitions) noexcept {
    if (!items::catalysts::valid(definitions) || !items::catalysts::matches_target_build(
                                                   runtime::persistence::context().buildIdentity)) {
        return false;
    }
    const items::catalysts::Facts facts = items::catalysts::generated_facts();
    std::array<bool, items::catalysts::kDefinitionCapacity> releasedFound{};
    for (const items::catalysts::Definition& catalyst : definitions) {
        items::Definition item{};
        items::Definition completedPlug{};
        items::Definition effect{};
        items::Definition acquisition{};
        items::details::Definition detail{};
        items::details::Definition effectDetail{};
        const auto released = std::lower_bound(facts.releasedWeaponHashes.begin(),
                                               facts.releasedWeaponHashes.end(),
                                               catalyst.itemDefinitionHash);
        const bool isReleased = released != facts.releasedWeaponHashes.end()
                                && *released == catalyst.itemDefinitionHash;
        if (isReleased) {
            const std::size_t index = static_cast<std::size_t>(
                released - facts.releasedWeaponHashes.begin());
            if (releasedFound[index]
                || catalyst.availability != items::catalysts::Availability::released) {
                return false;
            }
            releasedFound[index] = true;
        } else if (catalyst.availability == items::catalysts::Availability::released) {
            return false;
        }
        if (!items::find_index(catalyst.itemDefinitionIndex, item)
            || item.definitionHash != catalyst.itemDefinitionHash
            || !items::details::find(catalyst.itemDefinitionIndex, detail)
            || detail.definitionHash != catalyst.itemDefinitionHash
            || catalyst.socketLane >= detail.ordinarySocketCount) {
            return false;
        }
        if (catalyst.availability != items::catalysts::Availability::unsupported
            && !items::socket_plugs::allowed(catalyst.itemDefinitionIndex,
                                              catalyst.socketLane,
                                              catalyst.completedPlugDefinitionIndex)) {
            return false;
        }
        if (catalyst.availability != items::catalysts::Availability::unsupported
            && (!items::find_index(catalyst.completedPlugDefinitionIndex, completedPlug)
                || !items::find_index(catalyst.effectDefinitionIndex, effect)
                || !items::find_index(catalyst.acquisitionDefinitionIndex, acquisition)
                || !items::details::find(catalyst.effectDefinitionIndex, effectDetail)
                || effectDetail.definitionHash != effect.definitionHash
                || completedPlug.plugCategoryHash == 0
                || completedPlug.plugCategoryHash != effect.plugCategoryHash
                || effect.tier != static_cast<std::uint8_t>(items::Tier::exotic)
                || effectDetail.instancedDefinitionState
                       != items::details::InstancedDefinitionState::stackable
                || (effectDetail.sandboxPerkCount == 0 && effectDetail.statCount == 0))) {
            return false;
        }
    }
    return std::all_of(releasedFound.begin(),
                       releasedFound.begin() + facts.releasedWeaponHashes.size(),
                       [](bool found) { return found; });
}

} // namespace

bool exotic_catalysts_ready() noexcept {
    return items::catalysts::count() != 0;
}

bool derive_exotic_catalysts(
    std::span<const items::Definition> itemDefinitions,
    std::span<const items::details::Definition> itemDetails,
    std::span<const items::socket_plugs::Rule> socketPlugRules,
    std::span<const items::socket_plugs::Pool> socketPlugPools,
    std::span<const items::socket_plugs::Member> socketPlugMembers,
    std::span<const items::catalysts::CompletionCondition> completionConditions,
    std::span<const items::catalysts::AcquisitionGate> acquisitionGates,
    std::span<items::catalysts::Definition> output,
    std::size_t& count,
    items::catalysts::Report& report) noexcept {
    runtime::persistence::Context& state = runtime::persistence::context();
    AcquireSRWLockShared(&state.lock);
    const BuildIdentity build = state.buildIdentity;
    const bool enabled = state.enabled;
    ReleaseSRWLockShared(&state.lock);
    if (!enabled) {
        count = 0;
        report = {};
        report.error = items::catalysts::Error::unsupportedBuild;
        report.unsupported = 1;
        return false;
    }
    const items::catalysts::Source source{build,
                                          itemDefinitions,
                                          itemDetails,
                                          socketPlugRules,
                                          socketPlugPools,
                                          socketPlugMembers,
                                          completionConditions,
                                          acquisitionGates};
    return items::catalysts::derive(
        source, items::catalysts::generated_facts(), output, count, report);
}

bool publish_exotic_catalysts(
    std::span<const items::catalysts::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !valid_publication(definitions)) {
        return false;
    }
    return transaction.finish(items::catalysts::replace(definitions), items::catalysts::clear);
}

items::catalysts::ApplyResult complete_exotic_catalyst(
    std::uint16_t itemDefinitionIndex,
    std::uint32_t& flags,
    std::span<std::optional<std::uint16_t>> plugs) noexcept {
    return items::catalysts::apply_completed(itemDefinitionIndex, flags, plugs);
}

bool complete_exotic_catalyst_investment(Family5State& family) noexcept {
    return items::catalysts::append_investment_overrides(family);
}

std::uint16_t resolve_exotic_catalyst_effect(std::uint16_t itemDefinitionIndex,
                                             std::uint8_t socketLane,
                                             std::uint16_t plugDefinitionIndex) noexcept {
    return items::catalysts::resolve_effect(
        itemDefinitionIndex, socketLane, plugDefinitionIndex);
}

bool is_exotic_catalyst_lane(std::uint16_t itemDefinitionIndex,
                              std::uint8_t socketLane) noexcept {
    return items::catalysts::owns_lane(itemDefinitionIndex, socketLane);
}

items::catalysts::Report exotic_catalyst_report() noexcept {
    return items::catalysts::report();
}

void set_exotic_catalyst_completion_enabled(bool enabled) noexcept {
    items::catalysts::set_completion_enabled(enabled);
}

} // namespace sunrise::state::build_data
