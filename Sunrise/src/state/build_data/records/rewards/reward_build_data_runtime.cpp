#include "../../runtime.h"
#include "reward_catalog.h"

namespace sunrise::state::build_data {
namespace {

/** Carries one resolution attempt through the raw RowVisitor callback. */
struct ResolveContext {
    std::uint16_t itemDefinitionIndex{};
    std::int32_t quantity{};
    bool resolved{false};
};

/** Accepts the first visited row whose item hash resolves in this build's item table. */
bool resolve_first_installed(void* context, const records::rewards::RewardRow& row) noexcept {
    auto& resolution = *static_cast<ResolveContext*>(context);
    items::Definition item{};
    if (!find_item_definition_hash(row.itemHash, item)) {
        // Vaulted or later-era item this build never installed. Keep looking rather than failing
        // the whole record: the aggregate count of these is computed once when the table loads.
        return true;
    }
    resolution.itemDefinitionIndex = item.definitionIndex;
    resolution.quantity = static_cast<std::int32_t>(row.quantity);
    resolution.resolved = true;
    return false;
}

} // namespace

/** Finds one manifest-sourced reward for a claimed record, from the shipped generated table. */
bool find_generated_record_reward(std::uint32_t recordHash,
                                  std::uint16_t& itemDefinitionIndex,
                                  std::int32_t& quantity) noexcept {
    itemDefinitionIndex = 0;
    quantity = 0;
    ResolveContext resolution;
    records::rewards::visit_for_record(recordHash, resolve_first_installed, &resolution);
    if (!resolution.resolved) {
        return false;
    }
    itemDefinitionIndex = resolution.itemDefinitionIndex;
    quantity = resolution.quantity;
    return true;
}

} // namespace sunrise::state::build_data
