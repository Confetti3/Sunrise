#include "../../../../state/build_data/records/record_persistence.h"
#include "../../../../state/build_data/records/record_catalog.h"
#include "../../../../state/build_data/nodes/node_persistence.h"
#include <atomic>
#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/records/definition.h"
#include <cstdio>
#include <array>
#include <span>
#include <vector>
#include "../../../../state/build_data/nodes/node_catalog.h"
#include "../../../../state/record_claims/record_claims.h"
#include "account_encoder.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "../../../../state/build_data/runtime.h"
#include "../../../../state/unlocks/unlocks_runtime.h"
#include "../progression/progression_bank_keys.h"
#include "layout.h"
#include "preferences/preferences_encoder.h"

namespace sunrise::middleware::datagen::family4::account {
namespace {

/** Every bit set is the native empty biased 16-bit definition index. */
constexpr std::uint16_t kEmptyDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Signed 32-bit maximum keeps publicity deadlines beyond a normal session clock. */
constexpr std::uint64_t kSuppressedPublicityDeadline =
    static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
/** Every set bit marks all default account messages as already seen. */
constexpr std::byte kSeenMessageByte{0xFF};
/** A native inventory bucket id is 1 byte, so this covers every bucket. */
constexpr std::size_t kBucketIdentityCapacity = 256;

/**
 * Places one authored profile item in the slot run its inventory bucket owns.
 * The slot is not authored: the bucket descriptor names the first slot of its run, and items
 * sharing a bucket take consecutive slots in configuration order.
 * @param item Authored account-wide item.
 * @param taken Slots already claimed inside each bucket, indexed by bucket id.
 * @param rows Profile inventory rows.
 * @return True when the item resolves to a free profile slot.
 */
[[nodiscard]] bool place_profile_item(const state::account::inventory::ProfileItem& item,
                                      std::array<std::uint16_t, kBucketIdentityCapacity>& taken,
                                      std::span<inventory::layout::Entry> rows) noexcept {
    // The dense item table already carries the bucket, so a profile item needs no detail record.
    // Only equipped items and their plugs have one.
    state::build_data::items::Definition definition{};
    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (item.quantity <= 0 || item.mutationSerial < 0
        || !state::build_data::find_item_definition_hash(item.definitionHash, definition)
        || !state::build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || detail.instancedDefinitionState
               != state::build_data::items::details::InstancedDefinitionState::stackable
        || !state::build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
        || bucket.arraySelector != state::build_data::inventory::buckets::ArraySelector::profile) {
        return false;
    }
    const bool actionSource = state::build_data::is_profile_action_source(
        definition.definitionIndex, definition.bucketId);
    if (actionSource != (item.instanceSoid != 0)) {
        return false;
    }
    const std::uint16_t used = taken[definition.bucketId];
    if (used >= bucket.slotCount) {
        return false;
    }
    const std::size_t slot = static_cast<std::size_t>(bucket.firstSlot) + used;
    if (slot >= rows.size() || rows[slot].definitionIndex != kEmptyDefinitionIndex) {
        return false;
    }
    taken[definition.bucketId] = static_cast<std::uint16_t>(used + 1);
    rows[slot].definitionIndex = definition.definitionIndex;
    rows[slot].instanceSoid = item.instanceSoid;
    rows[slot].quantity = item.quantity;
    rows[slot].mutationSerial = item.mutationSerial;
    return true;
}

} // namespace

/** Encodes a sentinel-correct account object from authored State. */
bool encode(const state::AccountState& state, std::span<std::byte> output) noexcept {
    if (state.primarySoid == 0 || !state::account::valid(state)
        || output.size() < layout::kMinimumSize) {
        return false;
    }

    layout::Object object{};
    object.accountSoid = state.primarySoid;
    object.selectedCharacterSoid = state::account::selected_character_soid(state);
    if (!roster::initialize(state, object.roster)
        || !preferences::encode(state.settings, object.preferences, object.bindings)) {
        return false;
    }

    // Acquired flags and objective progress are authored policy, published once per process.
    const state::unlocks::Table& unlocks = state::unlocks::get();
    object.acquiredFlags = unlocks.accountFlags;
    object.profileUnlockFlags = unlocks.profileFlags;
    object.objectiveValues = unlocks.objectiveValues;
    // The node and record tables are not in the build data cache, so on a warm start the package
    // pass is skipped and both are empty. The account image needs them: without nodes no book gate
    // is satisfied and every book reads as unnamed. Publishing here, latched on success, is what
    // makes a warm start look like a cold one.
    {
        static std::atomic<bool> published{false};
        if (!published.load(std::memory_order_relaxed)) {
            const bool haveNodes = state::build_data::nodes::count() != 0
                                   || state::build_data::nodes::load_and_publish();
            const bool haveRecords = state::build_data::records::count() != 0
                                     || state::build_data::records::load_and_publish();
            if (haveNodes && haveRecords) {
                published.store(true, std::memory_order_relaxed);
            }
        }
    }

    // Settings predate collectible persistence and author many lore objectives at completion.
    // Clear every lore-owned record first; claims and collected progress are overlaid below.
    (void)state::record_claims::clear_lore_objectives(object.objectiveValues);
    // Claims are laid over the authored bank on the way out, so a claimed record reads Acquired on
    // the next image. The authored policy itself is immutable and is never edited.
    (void)state::record_claims::apply(object.acquiredFlags);
    // A lore book's category is gated: some read a flag, some test their own progress value. A book
    // gated on progress cannot open by being played, since with no title shown there is nothing
    // inside to collect. Satisfying the gate is what makes the book readable at all.
    (void)state::build_data::nodes::apply_visibility(object.acquiredFlags);
    // Triumph Score is a plain replicated value the client never derives, so total the claims made
    // this session over whatever the policy authored and publish the sum.
    if (state::build_data::records::kTriumphScoreValueIndex < object.objectiveValues.size()) {
        auto& score = object.objectiveValues[state::build_data::records::kTriumphScoreValueIndex];
        score += static_cast<std::int32_t>(state::record_claims::total_score());
    }
    // A node's progress bar reads a value slot and shows whatever it holds, so the claimed children
    // have to be counted into it here or the bar never moves.
    (void)state::record_claims::apply_node_progress(object.objectiveValues);
    // Early lore chapters have a second visibility value separate from their completion objective.
    // Publish it only for chapters the account actually holds, leaving undiscovered entries secret.
    (void)state::record_claims::apply_chapter_visibility_gates(object.objectiveValues);
    // A record reads claimable when its objective equals completionValue and its flag is clear --
    // the flag alone can never carry that state, so its objective value(s) are written here instead.
    (void)state::record_claims::apply_claimable_objectives(object.objectiveValues);
    // Eighteen lore books gate on a value slot instead of a flag. Run this after every other value
    // pass so an empty shared gate/bar receives its sentinel without overwriting a real count.
    (void)state::build_data::nodes::apply_category_gates(object.objectiveValues,
                                                        unlocks.revealAllLoreBooks);
    // Undiscovered chapters keep their authored visibility state. A former blanket gate fill made
    // whole books look claimable on a clean account, which bypassed collectible progression.

    for (layout::CharacterUnlockBlock& block : object.characterUnlocks) {
        block.flags = unlocks.characterFlags;
    }
    object.publicityExpiries.fill(kSuppressedPublicityDeadline);
    object.seenMessages.fill(kSeenMessageByte);
    for (inventory::layout::Entry& item : object.profileItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    for (inventory::layout::Entry& item : object.secondaryItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    if (!progression::key_bank(state::build_data::progressions::Scope::account,
                               object.progressions)) {
        return false;
    }
    // Profile rows are sentinelled above, so placement only has to claim its own slots.
    std::array<std::uint16_t, kBucketIdentityCapacity> takenSlots{};
    for (std::size_t index = 0; index < state.profileItemCount; ++index) {
        if (!place_profile_item(state.profileItems[index], takenSlots, object.profileItems)) {
            return false;
        }
    }
    object.profileItemCount = static_cast<std::uint32_t>(state.profileItemCount);

    // Commit only after every fallible conversion succeeds so callers never receive a partial
    // account object.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account
