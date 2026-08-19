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
Report g_report;
std::atomic<bool> g_completionEnabled{true};

/**
 * @param left First catalyst definition.
 * @param right Second catalyst definition.
 * @return True when the first native item index is less than the second.
 */
[[nodiscard]] bool less(const Definition& left, const Definition& right) noexcept {
    return left.itemDefinitionIndex < right.itemDefinitionIndex;
}

/**
 * Finds one item in a sorted catalog while its lock is held.
 * @param definitions Catalyst definitions in native item index order.
 * @param itemDefinitionIndex Native item index to find.
 * @return The matching definition, or null when no definition matches.
 */
[[nodiscard]] const Definition*
find(std::span<const Definition> definitions, std::uint16_t itemDefinitionIndex) noexcept {
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
    g_report = {};
    g_completionEnabled.store(true, std::memory_order_release);
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
        const bool hasEffect =
            definition.effectDefinitionIndex != details::kUnavailableItemIndex;
        if (definition.itemDefinitionHash == 0
            || definition.socketLane >= details::kInitialPlugCapacity
            || definition.reserved != 0
            || (definition.availability != Availability::released
                && definition.availability != Availability::placeholder
                && definition.availability != Availability::unsupported)
            || (definition.availability == Availability::unsupported
                && (hasCompletedPlug || hasEffect))
            || (definition.availability != Availability::unsupported
                && (!hasCompletedPlug || !hasEffect))
            || (index != 0 && !less(definitions[index - 1], definition))) {
            return false;
        }
    }
    return true;
}

bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    Report nextReport{};
    for (const Definition& definition : definitions) {
        if (definition.availability == Availability::released) {
            ++nextReport.released;
        } else if (definition.availability == Availability::placeholder) {
            ++nextReport.placeholder;
        } else {
            ++nextReport.unsupported;
        }
    }
    const Lock::Exclusive guard(g_lock);
    if (!g_definitions.replace(definitions)) {
        return false;
    }
    g_report = nextReport;
    return true;
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
    if (result.error != Error::none
        || !account::inventory::valid_item_state(flags)
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

bool snapshot(std::span<Definition> output,
              std::size_t& outputCount,
              Report& outputReport) noexcept {
    outputCount = 0;
    outputReport = {};
    const Lock::Shared guard(g_lock);
    if (!g_definitions.snapshot(output, outputCount)) {
        return false;
    }
    outputReport = g_report;
    return true;
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

Report report() noexcept {
    const Lock::Shared guard(g_lock);
    return g_report;
}

} // namespace sunrise::state::build_data::items::catalysts
