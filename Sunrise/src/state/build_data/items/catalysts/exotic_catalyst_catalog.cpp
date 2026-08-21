#include "exotic_catalyst_catalog.h"

#include <algorithm>
#include <atomic>

#include "../../../account/inventory/item_state.h"
#include "../../table.h"
#include "../details/definition.h"

namespace sunrise::state::build_data::items::catalysts {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
std::atomic<bool> g_completionEnabled{true};

/**
 * Finds one item in a sorted catalog while its lock is held.
 * @param definitions Catalyst definitions in native item index order.
 * @param itemDefinitionIndex Native item index to find.
 * @return The matching definition, or null when no definition matches.
 */
[[nodiscard]] const Definition* find(std::span<const Definition> definitions,
                                     std::uint16_t itemDefinitionIndex) noexcept {
    const auto found = std::lower_bound(
        definitions.begin(),
        definitions.end(),
        itemDefinitionIndex,
        [](const Definition& value, auto key) { return value.itemDefinitionIndex < key; });
    return found != definitions.end() && found->itemDefinitionIndex == itemDefinitionIndex
               ? &*found
               : nullptr;
}

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

void set_completion_enabled(bool enabled) noexcept {
    g_completionEnabled.store(enabled, std::memory_order_release);
}

bool completion_enabled() noexcept {
    return g_completionEnabled.load(std::memory_order_acquire);
}

bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const Definition& definition = definitions[index];
        const bool hasCompletedPlug =
            definition.completedPlugDefinitionIndex != details::kUnavailableItemIndex;
        const bool hasEffect = definition.effectDefinitionIndex != details::kUnavailableItemIndex;
        if (definition.itemDefinitionHash == 0
            || definition.socketLane >= details::kInitialPlugCapacity
            || !valid_availability(definition.availability)
            || (definition.availability == Availability::unsupported
                && (hasCompletedPlug || hasEffect))
            || (definition.availability != Availability::unsupported
                && (!hasCompletedPlug || !hasEffect))
            || (index != 0 && !definition_index_less(definitions[index - 1], definition))) {
            return false;
        }
    }
    return true;
}

bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_definitions.replace(definitions);
}

Result resolve(std::uint16_t itemDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    const Definition* definition = find(g_definitions.rows(), itemDefinitionIndex);
    if (definition == nullptr) {
        return {};
    }
    if (definition->availability == Availability::placeholder) {
        return {Error::placeholderOnly, Availability::placeholder, {}};
    }
    if (definition->availability != Availability::released) {
        return {Error::ambiguousLifecycle, Availability::unsupported, {}};
    }
    return {Error::none,
            Availability::released,
            {definition->socketLane,
             definition->completedPlugDefinitionIndex,
             definition->effectDefinitionIndex}};
}

std::uint16_t resolve_effect(std::uint16_t itemDefinitionIndex,
                             std::uint8_t socketLane,
                             std::uint16_t plugDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    const Definition* definition = find(g_definitions.rows(), itemDefinitionIndex);
    if (definition == nullptr || definition->availability != Availability::released
        || definition->socketLane != socketLane
        || definition->completedPlugDefinitionIndex != plugDefinitionIndex
        || definition->effectDefinitionIndex == details::kUnavailableItemIndex) {
        return plugDefinitionIndex;
    }
    return definition->effectDefinitionIndex;
}

bool owns_lane(std::uint16_t itemDefinitionIndex, std::uint8_t socketLane) noexcept {
    const Lock::Shared guard(g_lock);
    const Definition* definition = find(g_definitions.rows(), itemDefinitionIndex);
    return definition != nullptr && definition->socketLane == socketLane;
}

ApplyResult apply_completed(std::uint16_t itemDefinitionIndex,
                            std::uint32_t& flags,
                            std::span<std::optional<std::uint16_t>> plugs) noexcept {
    if (!completion_enabled()) {
        return ApplyResult::unchanged;
    }
    const Result result = resolve(itemDefinitionIndex);
    if (result.error == Error::noCatalyst || result.availability != Availability::released) {
        return ApplyResult::unchanged;
    }
    if (result.error != Error::none || !account::inventory::valid_item_state(flags)
        || result.completed.socketLane >= plugs.size()
        || result.completed.completedPlugDefinitionIndex == details::kUnavailableItemIndex) {
        return ApplyResult::failed;
    }

    const std::uint32_t completedFlags = flags | account::inventory::kMasterworkItemFlag;
    const std::optional<std::uint16_t> completedPlug =
        result.completed.completedPlugDefinitionIndex;
    plugs[result.completed.socketLane] = completedPlug;
    flags = completedFlags;
    return ApplyResult::completed;
}

bool snapshot(std::span<Definition> output, std::size_t& outputCount) noexcept {
    outputCount = 0;
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, outputCount);
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::items::catalysts
