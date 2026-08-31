#include <algorithm>

#include "sensor_auth_update.h"
#include "glimmer_extraction_contract.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;

/** The type-13 slot type, which is the only one that may carry the player key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
/** The participation region rides a signed field, so this is the widest index it accepts. */
constexpr std::uint32_t kMaximumRegion = 0x7FFFFFFF;
constexpr std::uint32_t kTrostlandGroup = 0x2986181D;
constexpr std::uint32_t kTrostlandSpawner = 0x80C26B0A;
constexpr std::uint8_t kTrostlandSlotType = 1;
constexpr std::uint16_t kTrostlandSlotIndex = 271;
namespace contract = glimmer_extraction;

[[nodiscard]] bool glimmer_spawn_slot(ContentStep step, std::uint16_t& slot) noexcept {
    switch (step) {
    case ContentStep::glimmerSite0ShipSpawn: slot = contract::kSites[0].dropship.slot; return true;
    case ContentStep::glimmerSite1ShipSpawn: slot = contract::kSites[1].dropship.slot; return true;
    case ContentStep::glimmerSite2ShipSpawn: slot = contract::kSites[2].dropship.slot; return true;
    default: return false;
    }
}

[[nodiscard]] bool glimmer_sequence_slot(ContentStep step, std::uint16_t& slot) noexcept {
    if (step == ContentStep::glimmerIntro) {
        slot = contract::kIntroSequence;
        return true;
    }
    switch (step) {
    case ContentStep::glimmerSite0Enter:
    case ContentStep::glimmerSite0Exit:
    case ContentStep::glimmerSite1Enter:
    case ContentStep::glimmerSite1Exit:
    case ContentStep::glimmerSite2Enter:
    case ContentStep::glimmerSite2Exit:
    case ContentStep::glimmerCleanup:
        slot = contract::kActiveSequence;
        return true;
    default: return false;
    }
}

[[nodiscard]] bool has_auth_slot(const Snapshot& snapshot,
                                 std::uint32_t key,
                                 std::uint8_t type,
                                 std::uint16_t index) noexcept {
    std::size_t matches = 0;
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.key != key) continue;
        for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
            if (row.slotTypes[slot] == type && row.slotIndices[slot] == index
                && (row.slotFlags[slot] & kSlotAuthFlag) != 0) {
                ++matches;
            }
        }
    }
    return matches == 1;
}

/**
 * Checks the per-bubble sub-blocks against what the client's own arrays hold.
 * An empty sub-block would publish a zero count and a zero mask, which registers nothing and
 * spends a slot, so it is refused rather than encoded.
 * @param subBlocks Sub-blocks the body would carry.
 * @return True when every sub-block names a usable bubble and a bounded key set.
 */
[[nodiscard]] bool valid_sub_blocks(std::span<const BubbleSubBlock> subBlocks) noexcept {
    if (subBlocks.size() > kBubbleSubBlockCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < subBlocks.size(); ++index) {
        const BubbleSubBlock& block = subBlocks[index];
        if (block.bubble > kMaximumSubBlockBubble || block.keys.empty()
            || block.keys.size() > kBubbleKeyCapacity) {
            return false;
        }
        // The client's array holds one element per bubble, and its sweep walks every element that
        // matches. A repeated bubble would register the same keys twice in one apply.
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (subBlocks[earlier].bubble == block.bubble) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Checks the scalars whose out-of-range values would encode with no complaint.
 * @param snapshot Message input.
 * @return True when every scalar fits its field.
 */
[[nodiscard]] bool valid(const Snapshot& snapshot) noexcept {
    if (std::find(kLifetimeStates.begin(), kLifetimeStates.end(), snapshot.lifetime)
        == kLifetimeStates.end()) {
        return false;
    }
    if (snapshot.hasRegion && snapshot.region > kMaximumRegion) {
        return false;
    }
    if (snapshot.hasSpawnOverride
        && (snapshot.spawnSliceSet > kMaximumSpawnSliceSet || snapshot.spawnSetHash == 0
            || snapshot.spawnSetHash == kAbsentSpawnSetHash)) {
        return false;
    }
    // The grant is a change, not a value: the client compares it against a mirror that starts at
    // zero, so a token of zero grants nothing.
    if (snapshot.hasGrant
        && (snapshot.grant.bubble > kMaximumGrantBubble
            || snapshot.grant.token < kMinimumGrantToken)) {
        return false;
    }
    if (snapshot.roster.groupCount > kGroupCapacity
        || snapshot.roster.topLevelGroupCount > snapshot.roster.groupCount) {
        return false;
    }
    for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        if (row.slotTypes.size() != row.slotFlags.size()
            || row.slotTypes.size() != row.slotIndices.size() || row.slotTypes.empty()) {
            return false;
        }
        // An index past the field's range wraps into another slot's, which seeds the wrong
        // object rather than refusing.
        for (const std::uint16_t index : row.slotIndices) {
            if (index > kMaximumSlotIndex) {
                return false;
            }
        }
    }
    if (snapshot.hasSense) {
        if (snapshot.sense.group != kTrostlandGroup
            || snapshot.sense.definition != kTrostlandSpawner
            || snapshot.sense.slotType != kTrostlandSlotType
            || snapshot.sense.slotIndex != kTrostlandSlotIndex || snapshot.sense.mode != 0
            || snapshot.sense.requested != std::array<std::uint32_t, 2>{1, 0}
            || snapshot.sense.generation == 0 || snapshot.sense.generation > 0x7FFFFFFFU
            || snapshot.sense.deltaWidth == 0
            || snapshot.sense.deltaWidth > 96) {
            return false;
        }
        std::size_t matches = 0;
        for (std::size_t group = 0; group < snapshot.roster.groupCount; ++group) {
            const Group& row = snapshot.roster.groups[group];
            if (row.key != snapshot.sense.group) continue;
            for (std::size_t slot = 0; slot < row.slotTypes.size(); ++slot) {
                if (row.slotTypes[slot] == snapshot.sense.slotType
                    && row.slotIndices[slot] == snapshot.sense.slotIndex
                    && (row.slotFlags[slot] & kSlotAuthFlag) != 0) ++matches;
            }
        }
        if (matches != 1) return false;
    }
    if (snapshot.hasContentStep) {
        if (snapshot.contentStep.generation == 0
            || snapshot.contentStep.generation > 0x7FFFFFFFU) return false;
        std::uint16_t slot = 0;
        if (glimmer_spawn_slot(snapshot.contentStep.step, slot)) {
            if (!has_auth_slot(snapshot, contract::kGroup, contract::kSpawnerType, slot)) {
                return false;
            }
        } else if (glimmer_sequence_slot(snapshot.contentStep.step, slot)) {
            if (!has_auth_slot(snapshot, contract::kGroup, contract::kSequenceType, slot)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return valid_sub_blocks(snapshot.roster.bubbleSubBlocks);
}

/**
 * Writes every group's object blocks, in publish order, per-bubble groups included.
 * Every registered object must be seeded before any auth state applies, because the client's gate
 * walks the whole sync-record pool. A partial message seeds nothing that applies.
 * @param writer Body writer sitting after the phase-1 delta.
 * @param snapshot Message input.
 * @return True when every block fits.
 */
[[nodiscard]] bool write_phase_two(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    bool encoded = true;
    bool keyPlaced = false;
    for (std::size_t group = 0; encoded && group < snapshot.roster.groupCount; ++group) {
        const Group& row = snapshot.roster.groups[group];
        // The filler word after the key is read and discarded.
        encoded = writer.write(1, kPresenceWidth) && writer.write(row.key, kKeyWidth)
                  && writer.write(0, kKeyWidth);
        for (std::size_t slot = 0; encoded && slot < row.slotTypes.size(); ++slot) {
            const std::uint8_t slotType = row.slotTypes[slot];
            const bool firstOrEvery = !keyPlaced || snapshot.keyOnEveryParticipationSlot;
            const bool carriesPlayerKey = slotType == kSlotTypeParticipation
                                          && row.key == snapshot.roster.playerKeyGroup
                                          && firstOrEvery;
            keyPlaced = keyPlaced || carriesPlayerKey;
            encoded = write_object_block(writer,
                                         snapshot,
                                         row.key,
                                         slotType,
                                         row.slotIndices[slot],
                                         row.slotFlags[slot],
                                         carriesPlayerKey);
        }
        encoded = encoded && writer.write(0, kPresenceWidth);
    }
    return encoded;
}

/**
 * Writes the whole body through one writer.
 * @param writer Real or measuring writer positioned at the first bit.
 * @param snapshot Message input.
 * @return True when every field fit.
 */
[[nodiscard]] bool write_body(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // The hardwipe token is unchecked unless the client's `use_hardwipe_tokens` config is on.
    bool encoded = writer.write(0, kHardwipeWidth)
                   && writer.write(snapshot.patchEpoch.first, kEpochWidth)
                   && writer.write(snapshot.patchEpoch.second, kEpochWidth)
                   && writer.write(snapshot.hasGrant ? 1U : 0U, kPresenceWidth);
    if (encoded && snapshot.hasGrant) {
        encoded = write_bubble_block(writer, snapshot.grant);
    }
    const std::size_t latchBit = kLatchBitWithoutGrant + (snapshot.hasGrant ? kBubbleBlockBits : 0);
    // The token at the activity object's element 10 is not checked.
    encoded = encoded && writer.write(0, kActivityTokenWidth) && writer.bit_count() == latchBit;
    // The enable latch is not sticky, so it goes on every message.
    encoded = encoded && writer.write(1, kPresenceWidth)
              && write_roster_delta(writer, snapshot.roster, snapshot.stateSequence)
              && writer.bit_count()
                     == latchBit + 1
                            + delta_bits(snapshot.roster.topLevelGroupCount,
                                         snapshot.roster.bubbleSubBlocks);
    if (encoded && !snapshot.phaseOneOnly) {
        encoded = write_phase_two(writer, snapshot);
    }
    // The entity-group loop end, then the trailing pair, which short-circuits to one bit.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth);
}

} // namespace

/** Encodes one `sensor_auth_update` body. */
bool encode_sensor_auth_update(const Snapshot& snapshot,
                               std::span<std::byte> output,
                               std::size_t& written) noexcept {
    written = 0;
    if (output.empty() || !valid(snapshot)) {
        return false;
    }

    // Measure first. The writer clears and fills the caller's storage as it goes, so a body that
    // does not fit would leave a partial one behind.
    bits::Writer measure = bits::Writer::measuring();
    std::size_t required = 0;
    if (!write_body(measure, snapshot) || !measure.finish(required) || required > output.size()) {
        return false;
    }

    bits::Writer writer(output);
    std::size_t produced = 0;
    if (!write_body(writer, snapshot) || !writer.finish(produced) || produced != required) {
        return false;
    }
    written = produced;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
