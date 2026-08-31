#include "sensor_auth_update.h"
#include "glimmer_extraction_contract.h"

namespace sunrise::middleware::bap::activity_message::sensor_auth_update {
namespace {

namespace bits = encoding::bits;

/** Slot types whose auth body this module fills. Every other block is seed-only. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
constexpr std::uint8_t kSlotTypeLifetime = 17;
constexpr std::uint8_t kSlotTypeConfiguration = 8;
constexpr std::uint8_t kSlotTypeSpawner = 1;
constexpr std::uint8_t kSlotTypePackage = 16;
constexpr std::uint8_t kSlotTypeQueues = 41;
constexpr std::uint8_t kSlotTypeSpawnKeys = 67;
constexpr std::uint8_t kSlotTypeSequence = 5;
namespace contract = glimmer_extraction;

/** Body widths, each checked against the writer after the body is written. */
constexpr std::size_t kParticipationBits = 192;
constexpr std::size_t kParticipationRegionBits = 32;
constexpr std::size_t kLifetimeBits = 520;
constexpr std::size_t kConfigurationBits = 35;
constexpr std::size_t kPackageBits = 7;
constexpr std::size_t kQueueBits = 12;
constexpr std::size_t kSpawnKeyBits = 32 * 32 + 1 + 32;
/**
 * Build-86657 `0x80807EC9`: the bounded Trostland spawner auth body below.
 * The root bit is owned by the object block and is not included here.
 */
constexpr std::size_t kSpawnerBits = 278;
/** Exact build-86657 `0x80804F04` body: 2x u64, u8, 4x u32, 128 references, one reference. */
constexpr std::size_t kSequenceBits = 2 * 64 + 8 + 4 * 32 + 128 * 55 + 55;

/** Signed fields in these bodies carry a -2^31 bias, so this wire value stores zero. */
constexpr std::uint32_t kSignedZero = 0x80000000;
/** The same bias wraps at the top of the field, so this wire value stores -1. */
constexpr std::uint32_t kSignedMinusOne = 0x7FFFFFFF;
/** The region index rides the same bias, so its wire value is the bias plus the index. */
constexpr std::uint32_t kRegionBias = 0x80000000;
/** Message 52's team-state byte 1, where bit 1 is `awaiting_client_sync`. */
constexpr std::uint32_t kAwaitingClientSync = 2;
/** Neutral runtime-i32 override that forces the type-17 waiting selector to zero. */
constexpr std::uint32_t kWaitingSwitchKey = 0xB3C1251B;
constexpr std::uint32_t kWaitingSwitchClass = 0x80800007;
/** Type 17 carries 3 spawn overrides. Wire zero stores index -1 and disables one. */
constexpr std::size_t kSpawnOverrideCount = 3;
constexpr std::uint8_t kSpawnOverrideIndexWidth = 10;
constexpr std::uint32_t kSpawnOverrideIndexBias = 1;
/** Type 67 maps the 32 spawn-key ordinals to themselves, matching its constructor. */
constexpr std::size_t kSpawnKeyCount = 32;

/**
 * Writes the participation body, which binds the player and latches the region. Zero-fill is not
 * safe here. Every biased field must carry its bias, or a stored zero decodes to the smallest
 * signed value.
 * @param writer Body writer.
 * @param snapshot Message input.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_participation(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    // An optional field's value follows its presence bit, so sending +0 shifts everything below.
    bool encoded = writer.write(snapshot.hasRegion ? 1U : 0U, kPresenceWidth);
    if (encoded && snapshot.hasRegion) {
        encoded = writer.write(kRegionBias + snapshot.region, kParticipationRegionBits);
    }
    // The participation record is this body's head, so struct +8 and +10 are record +8 and +10.
    // Record +8 is step 36 task 9's own term and +10 is the spawn gate's.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(1, kPresenceWidth)
           && writer.write(1, kPresenceWidth) && writer.write(1, kPresenceWidth)
           && writer.write(0, kPresenceWidth) && writer.write(1, 3) && writer.write(1, 2)
           && writer.write(0, 3) && writer.write(0, 32) && writer.write(1, 5)
           && writer.write(0, kPresenceWidth) && writer.write(0, 3)
           && writer.write(1, kPresenceWidth) && writer.write(snapshot.playerKey, 64)
           && writer.write(0, 5) && writer.write(3, 6) && writer.write(0, 6)
           && writer.write(0, 6)
           // Byte 736 skips the respawn delay, whose countdown never expires when the content
           // delay is negative. Byte 737 holds the spawn while the client loads.
           && writer.write(1, kPresenceWidth)
           && writer.write(snapshot.awaitClientSync ? kAwaitingClientSync : 0U, 4)
           && writer.write(0, 3) && writer.write(0, kPresenceWidth) && writer.write(128, 8)
           && writer.write(kSignedZero, 32);
}

/**
 * Writes the lifetime body, which is the activity state the roster reports.
 * @param writer Body writer.
 * @param snapshot Message input.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_lifetime(bits::Writer& writer, const Snapshot& snapshot) noexcept {
    bool encoded = writer.write(std::uint32_t{snapshot.lifetime} + 1, 4) && writer.write(1, 3)
                   && writer.write(0, kPresenceWidth) && writer.write(kSignedZero, 32)
                   && writer.write(0, 32) && writer.write(kSignedZero, 32) && writer.write(1, 6)
                   && writer.write(kWaitingSwitchKey, 32) && writer.write(1, kPresenceWidth)
                   && writer.write(kWaitingSwitchClass, 32) && writer.write(kSignedZero, 32)
                   && writer.write(kSignedZero, 32);
    for (std::size_t index = 0; encoded && index < kSpawnOverrideCount; ++index) {
        const std::uint32_t slice =
            snapshot.hasSpawnOverride ? snapshot.spawnSliceSet + kSpawnOverrideIndexBias : 0U;
        const std::uint32_t hash =
            snapshot.hasSpawnOverride ? snapshot.spawnSetHash : kAbsentSpawnSetHash;
        encoded = writer.write(slice, kSpawnOverrideIndexWidth) && writer.write(hash, 32);
    }
    // Struct `+1256` is the out-of-bounds `activity_quarantine` selector. The reader arms the
    // quarantine at or below 0x3F unsigned, so minus one leaves it clear and teleports nobody.
    return encoded && writer.write(0, kPresenceWidth) && writer.write(0, 32)
           && writer.write(kSignedMinusOne, 32) && writer.write(0, 32)
           && writer.write(kSlotTypeBias, kSlotTypeWidth)
           && writer.write(kSlotIndexBias, kSlotIndexWidth) && writer.write(0, 32)
           && writer.write(0, 3);
}

/**
 * Writes the spawn-key body, which maps the 32 ordinals to themselves.
 * @param writer Body writer.
 * @return True when the body fits.
 */
[[nodiscard]] bool write_spawn_keys(bits::Writer& writer) noexcept {
    bool encoded = true;
    for (std::size_t index = 0; encoded && index < kSpawnKeyCount; ++index) {
        encoded = writer.write(kSignedZero + index, 32);
    }
    return encoded && writer.write(0, kPresenceWidth) && writer.write(kSignedMinusOne, 32);
}

/** Writes one allowlisted Trostland `{1,0}` spawner state under auth schema `0x80807EC9`. */
[[nodiscard]] bool write_spawner(bits::Writer& writer,
                                 std::uint32_t generation,
                                 std::uint32_t group,
                                 std::uint16_t ruleSlotIndex) noexcept {
    constexpr std::uint32_t kSlotCount = 2;
    constexpr std::uint8_t kSlotCountWidth = 4;
    constexpr std::uint8_t kRuleSlotType = 66;

    // Optional keys A/B and the pad list are absent. The requested and reserve arrays are the
    // two dynamic `0x80807ECF` fields: a 4-bit count followed by signed i32 elements.
    bool encoded = writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                   && writer.write(0, kPresenceWidth) && writer.write(1, kPresenceWidth)
                   && writer.write(kSlotCount, kSlotCountWidth)
                   && writer.write(kSignedZero + 1, 32)
                   && writer.write(kSignedZero, 32)
                   && writer.write(1, kPresenceWidth)
                   && writer.write(kSlotCount, kSlotCountWidth)
                   && writer.write(kSignedZero, 32) && writer.write(kSignedZero, 32)
                   // The five-byte flag record is absent; spawn generation is present.
                   && writer.write(0, kPresenceWidth) && writer.write(1, kPresenceWidth)
                   && writer.write(generation, 31)
                   // Unknown u32s, target keys, and target reference are absent.
                   && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                   && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                   // The squad key is explicit and selected only by the build-scoped caller.
                   && writer.write(1, kPresenceWidth) && writer.write(group, 32)
                   && writer.write(std::uint32_t{kRuleSlotType} + kSlotTypeBias,
                                   kSlotTypeWidth)
                   && writer.write(std::uint32_t{ruleSlotIndex} + kSlotIndexBias,
                                   kSlotIndexWidth)
                   // Second reference and five optional scalar fields are absent.
                   && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                   && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                   && writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                   // `active=true`, `mode=0`; both fields are unconditional and bias 1.
                   && writer.write(2, 2) && writer.write(1, 3)
                   // An explicit no-name hash avoids turning an absent optional into raw zero.
                   && writer.write(1, kPresenceWidth)
                   && writer.write(kAbsentSpawnSetHash, 32);
    return encoded;
}

[[nodiscard]] bool write_object_reference(bits::Writer& writer,
                                          std::uint32_t key,
                                          std::int8_t type,
                                          std::int16_t index) noexcept {
    return writer.write(key, 32)
           && writer.write(static_cast<std::uint32_t>(static_cast<std::int32_t>(type) + 1), 7)
           && writer.write(static_cast<std::uint32_t>(static_cast<std::int32_t>(index) + 32768),
                           16);
}

/**
 * Pulses one package-authored event sequence through its configured site-0 command target.
 * Build 86657 declares the intro and event-active controllers separately, with the intro ordered
 * immediately before the active controller in the roster. Both use the same exact auth schema.
 * Every array member is unconditional in `0x80804F04`; unused references are explicit -1 refs.
 */
[[nodiscard]] bool glimmer_sequence(ContentStep step,
                                    std::uint16_t& controller,
                                    std::uint16_t& command) noexcept {
    controller = step == ContentStep::glimmerIntro ? contract::kIntroSequence
                                                   : contract::kActiveSequence;
    switch (step) {
    case ContentStep::glimmerIntro:
    case ContentStep::glimmerSite0Enter: command = contract::kSites[0].enterCommand.slot; return true;
    case ContentStep::glimmerSite0Exit: command = contract::kSites[0].exitCommand.slot; return true;
    case ContentStep::glimmerSite1Enter: command = contract::kSites[1].enterCommand.slot; return true;
    case ContentStep::glimmerSite1Exit: command = contract::kSites[1].exitCommand.slot; return true;
    case ContentStep::glimmerSite2Enter: command = contract::kSites[2].enterCommand.slot; return true;
    case ContentStep::glimmerSite2Exit: command = contract::kSites[2].exitCommand.slot; return true;
    case ContentStep::glimmerCleanup: command = contract::kSites[2].exitCommand.slot; return true;
    default: return false;
    }
}

[[nodiscard]] bool glimmer_spawn(ContentStep step,
                                 std::uint16_t& spawner,
                                 std::uint16_t& rule) noexcept {
    switch (step) {
    case ContentStep::glimmerSite0ShipSpawn:
        spawner = contract::kSites[0].dropship.slot; rule = contract::kSites[0].dropshipRule.slot; return true;
    case ContentStep::glimmerSite1ShipSpawn:
        spawner = contract::kSites[1].dropship.slot; rule = contract::kSites[1].dropshipRule.slot; return true;
    case ContentStep::glimmerSite2ShipSpawn:
        spawner = contract::kSites[2].dropship.slot; rule = contract::kSites[2].dropshipRule.slot; return true;
    default: return false;
    }
}

[[nodiscard]] bool write_glimmer_site_transition(bits::Writer& writer,
                                                 const Snapshot& snapshot) noexcept {
    std::uint16_t controller = 0;
    std::uint16_t command = 0;
    if (!glimmer_sequence(snapshot.contentStep.step, controller, command)) return false;
    const std::uint32_t generation = snapshot.contentStep.generation;
    bool encoded = writer.write(generation, 64) && writer.write(0, 64)
                   && writer.write(1, 8) && writer.write(generation, 32)
                   && writer.write(0, 32) && writer.write(0, 32) && writer.write(0, 32);
    for (std::size_t index = 0; encoded && index < 128; ++index) {
        encoded = index == 0
                      ? write_object_reference(writer,
                                               contract::kGroup,
                                               contract::kCommandType,
                                               static_cast<std::int16_t>(command))
                      : write_object_reference(writer, 0, -1, -1);
    }
    return encoded && write_object_reference(writer, 0, -1, -1);
}

} // namespace

/** Reports how many bits of auth body one slot carries. */
std::size_t auth_body_bits(const Snapshot& snapshot,
                           std::uint32_t key,
                           std::uint8_t slotType,
                           std::uint16_t slotIndex,
                           bool carriesPlayerKey) noexcept {
    if (slotType == kSlotTypeSpawner && snapshot.hasSense && key == snapshot.sense.group
        && slotIndex == snapshot.sense.slotIndex) {
        return kSpawnerBits;
    }
    std::uint16_t spawner = 0;
    std::uint16_t rule = 0;
    if (slotType == kSlotTypeSpawner && snapshot.hasContentStep
        && key == contract::kGroup
        && glimmer_spawn(snapshot.contentStep.step, spawner, rule) && slotIndex == spawner) {
        return kSpawnerBits;
    }
    std::uint16_t controller = 0;
    std::uint16_t command = 0;
    if (slotType == kSlotTypeSequence && snapshot.hasContentStep && key == contract::kGroup
        && glimmer_sequence(snapshot.contentStep.step, controller, command)
        && slotIndex == controller) {
        return kSequenceBits;
    }
    if (slotType == kSlotTypeParticipation) {
        return carriesPlayerKey
                   ? kParticipationBits + (snapshot.hasRegion ? kParticipationRegionBits : 0)
                   : 0;
    }
    if (slotType == kSlotTypeLifetime) {
        return kLifetimeBits;
    }
    if (slotType == kSlotTypeConfiguration) {
        return kConfigurationBits;
    }
    if (slotType == kSlotTypePackage) {
        return kPackageBits;
    }
    if (slotType == kSlotTypeQueues) {
        return kQueueBits;
    }
    if (slotType == kSlotTypeSpawnKeys) {
        return kSpawnKeyBits;
    }
    return 0;
}

/** Writes one slot's auth body. */
bool write_auth_body(bits::Writer& writer,
                     const Snapshot& snapshot,
                     std::uint32_t key,
                     std::uint8_t slotType,
                     std::uint16_t slotIndex,
                     bool carriesPlayerKey) noexcept {
    const std::size_t start = writer.bit_count();
    const std::size_t expected =
        auth_body_bits(snapshot, key, slotType, slotIndex, carriesPlayerKey);
    std::uint16_t spawner = 0;
    std::uint16_t rule = 0;
    std::uint16_t controller = 0;
    std::uint16_t command = 0;
    bool encoded = true;
    if (slotType == kSlotTypeParticipation && carriesPlayerKey) {
        encoded = write_participation(writer, snapshot);
    } else if (slotType == kSlotTypeSpawner && snapshot.hasSense
               && key == snapshot.sense.group && slotIndex == snapshot.sense.slotIndex) {
        encoded = write_spawner(writer, snapshot.sense.generation, snapshot.sense.group, 565);
    } else if (slotType == kSlotTypeSpawner && snapshot.hasContentStep
               && key == contract::kGroup
               && glimmer_spawn(snapshot.contentStep.step, spawner, rule)
               && slotIndex == spawner) {
        encoded = write_spawner(writer,
                                snapshot.contentStep.generation,
                                contract::kGroup,
                                rule);
    } else if (slotType == kSlotTypeSequence && snapshot.hasContentStep
               && key == contract::kGroup
               && glimmer_sequence(snapshot.contentStep.step, controller, command)
               && slotIndex == controller) {
        encoded = write_glimmer_site_transition(writer, snapshot);
    } else if (slotType == kSlotTypeLifetime) {
        encoded = write_lifetime(writer, snapshot);
    } else if (slotType == kSlotTypeConfiguration) {
        // Both optional arrays absent and the terminal tag clear is the constructed state.
        encoded = writer.write(0, kPresenceWidth) && writer.write(0, kPresenceWidth)
                  && writer.write(0, kPresenceWidth) && writer.write(0, 32);
    } else if (slotType == kSlotTypePackage) {
        // 7 absent top-level fields keep the package-owned configuration.
        encoded = pad_bits(writer, kPackageBits);
    } else if (slotType == kSlotTypeQueues) {
        encoded = writer.write(0, 7) && writer.write(0, 5);
    } else if (slotType == kSlotTypeSpawnKeys) {
        encoded = write_spawn_keys(writer);
    }
    return encoded && writer.bit_count() == start + expected;
}

} // namespace sunrise::middleware::bap::activity_message::sensor_auth_update
