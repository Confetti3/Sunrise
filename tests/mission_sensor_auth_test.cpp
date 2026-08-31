#include <array>
#include <cassert>
#include <string_view>

#include "middleware/bap/activity_message/sense_update.h"
#include "middleware/bap/activity_message/sensor_auth_update.h"
#include "middleware/encoding/bit_reader.h"

using namespace sunrise::middleware::bap::activity_message::sensor_auth_update;

int main() {
    std::array<std::byte, 64> senseEnvelopeStorage{};
    sunrise::middleware::encoding::bits::Writer senseEnvelopeWriter(senseEnvelopeStorage);
    assert(senseEnvelopeWriter.write(0x1122334455667788ULL, 64));
    assert(senseEnvelopeWriter.write(0x99AABBCCDDEEFF00ULL, 64));
    assert(senseEnvelopeWriter.write(0, 1)); // outer literal
    assert(senseEnvelopeWriter.write(0, 1)); // global delta absent
    assert(senseEnvelopeWriter.write(1, 1));
    assert(senseEnvelopeWriter.write(0x2986181D, 32));
    assert(senseEnvelopeWriter.write(3, 32));
    assert(senseEnvelopeWriter.write(5, 3));
    assert(senseEnvelopeWriter.write(1, 1));
    assert(senseEnvelopeWriter.write(0x4786C0E0, 32));
    assert(senseEnvelopeWriter.write(1, 32));
    assert(senseEnvelopeWriter.write(0, 1));
    assert(senseEnvelopeWriter.write(0, 1)); // end groups
    assert(senseEnvelopeWriter.write(0, 1)); // final literal
    std::size_t senseEnvelopeBytes = 0;
    assert(senseEnvelopeWriter.finish(senseEnvelopeBytes));
    sunrise::middleware::bap::activity_message::sense_update::Envelope senseEnvelope{};
    std::size_t senseEnvelopeBits = 0;
    assert(sunrise::middleware::bap::activity_message::sense_update::parse_envelope(
        std::span<const std::byte>(senseEnvelopeStorage.data(), senseEnvelopeBytes),
        senseEnvelope,
        senseEnvelopeBits));
    assert(senseEnvelope.complete && !senseEnvelope.globalDeltaPresent);
    assert(senseEnvelope.groupCount == 2);
    assert(senseEnvelope.groups[0].key == 0x2986181D && senseEnvelope.groups[0].bits == 3);
    assert(senseEnvelope.groups[1].key == 0x4786C0E0 && senseEnvelope.groups[1].bits == 1);

    const std::array<std::uint8_t, 1> types{1};
    const std::array<std::uint8_t, 1> flags{kSlotSenseFlag | kSlotAuthFlag};
    const std::array<std::uint16_t, 1> indices{271};
    Snapshot snapshot{};
    snapshot.patchEpoch = {~std::uint64_t{}, ~std::uint64_t{}};
    snapshot.roster.groups[0] = {0x2986181D, types, flags, indices};
    snapshot.roster.groupCount = 1;
    snapshot.roster.topLevelGroupCount = 1;
    snapshot.roster.playerKeyGroup = 0x2986181D;
    snapshot.lifetime = 3;

    std::array<std::byte, 1024> baseline{};
    std::size_t baselineBytes = 0;
    assert(encode_sensor_auth_update(snapshot, baseline, baselineBytes));

    snapshot.hasSense = true;
    snapshot.sense.group = 0x2986181D;
    snapshot.sense.definition = 0x80C26B0A;
    snapshot.sense.generation = 3;
    snapshot.sense.slotType = 1;
    snapshot.sense.slotIndex = 271;
    snapshot.sense.mode = 0;
    snapshot.sense.requested = {1, 0};
    std::array<std::byte, 1024> contributed{};
    std::size_t contributedBytes = 0;
    assert(encode_sensor_auth_update(snapshot, contributed, contributedBytes));
    assert(contributedBytes > baselineBytes);
    assert(contributed != baseline);
    assert(auth_body_bits(snapshot, 0x2986181D, 1, 271, false) != 0);
    assert(auth_body_bits(snapshot, 0x2986181D, 1, 272, false) == 0);
    assert(auth_body_bits(snapshot, 0x2986181C, 1, 271, false) == 0);

    // A contribution is scoped to its exact roster identity. Other type-1 slots are seeded but
    // must not receive the spawner auth body.
    std::array<std::byte, 128> scopedBlocks{};
    sunrise::middleware::encoding::bits::Writer scopedWriter(scopedBlocks);
    assert(write_object_block(scopedWriter,
                              snapshot,
                              0x2986181D,
                              1,
                              272,
                              kSlotSenseFlag | kSlotAuthFlag,
                              false));
    assert(write_object_block(scopedWriter,
                              snapshot,
                              0x2986181D,
                              1,
                              271,
                              kSlotSenseFlag | kSlotAuthFlag,
                              false));
    std::size_t scopedBytes = 0;
    assert(scopedWriter.finish(scopedBytes));
    sunrise::middleware::encoding::bits::Reader scopedReader(
        std::span<const std::byte>(scopedBlocks.data(), scopedBytes));
    std::uint64_t scopedField = 0;
    assert(scopedReader.skip(56));
    assert(scopedReader.read(32, scopedField) && scopedField == 3);
    assert(scopedReader.skip(3 + 56));
    assert(scopedReader.read(32, scopedField) && scopedField == 281);

    // One-group fixture: phase 2 begins at bit 522 and the object's 32-bit remainder at 643.
    sunrise::middleware::encoding::bits::Reader authReader(
        std::span<const std::byte>(contributed.data(), contributedBytes));
    std::uint64_t field = 0;
    assert(authReader.skip(643));
    assert(authReader.read(32, field) && field == 281);
    assert(authReader.read(1, field) && field == 1); // auth reset
    assert(authReader.read(1, field) && field == 1); // auth root
    assert(authReader.read(1, field) && field == 0); // key A absent
    assert(authReader.read(1, field) && field == 0); // key B absent
    assert(authReader.read(1, field) && field == 0); // pad list absent
    assert(authReader.read(1, field) && field == 1); // requested array present
    assert(authReader.read(4, field) && field == 2);
    assert(authReader.read(32, field) && field == 0x80000001);
    assert(authReader.read(32, field) && field == 0x80000000);
    assert(authReader.read(1, field) && field == 1); // reserve array present
    assert(authReader.read(4, field) && field == 2);
    assert(authReader.read(32, field) && field == 0x80000000);
    assert(authReader.read(32, field) && field == 0x80000000);
    assert(authReader.read(1, field) && field == 0); // flag record absent
    assert(authReader.read(1, field) && field == 1); // spawn generation present
    assert(authReader.read(31, field) && field == 3);
    for (std::size_t index = 0; index < 4; ++index) {
        assert(authReader.read(1, field) && field == 0);
    }
    assert(authReader.read(1, field) && field == 1); // squad key present
    assert(authReader.read(32, field) && field == 0x2986181D);
    assert(authReader.read(7, field) && field == 67);
    assert(authReader.read(16, field) && field == 32768 + 565);
    for (std::size_t index = 0; index < 6; ++index) {
        assert(authReader.read(1, field) && field == 0);
    }
    assert(authReader.read(2, field) && field == 2); // active = true, bias 1
    assert(authReader.read(3, field) && field == 1); // mode = 0, bias 1
    assert(authReader.read(1, field) && field == 1); // no-name hash present
    assert(authReader.read(32, field) && field == kAbsentSpawnSetHash);
    assert(authReader.read(1, field) && field == 0); // type-5 sense mirror untouched

    Snapshot intro{};
    intro.lifetime = 3;
    intro.hasContentStep = true;
    intro.contentStep.step = ContentStep::glimmerIntro;
    intro.contentStep.generation = 6;
    assert(auth_body_bits(intro, 0x1F6C5054, 5, 13, false) == 7359);
    assert(auth_body_bits(intro, 0x1F6C5054, 5, 14, false) == 0);
    std::array<std::byte, 1024> introBody{};
    sunrise::middleware::encoding::bits::Writer introWriter(introBody);
    assert(write_auth_body(introWriter, intro, 0x1F6C5054, 5, 13, false));
    std::size_t introBytes = 0;
    assert(introWriter.finish(introBytes) && introBytes == 920);
    sunrise::middleware::encoding::bits::Reader introReader(
        std::span<const std::byte>(introBody.data(), introBytes));
    assert(introReader.skip(296));
    assert(introReader.read(7, field) && field == 59);
    assert(introReader.read(16, field) && field == 32768 + 394);

    Snapshot sequence{};
    sequence.lifetime = 3;
    sequence.hasContentStep = true;
    sequence.contentStep.step = ContentStep::glimmerSite0Enter;
    sequence.contentStep.generation = 7;
    assert(auth_body_bits(sequence, 0x1F6C5054, 5, 13, false) == 0);
    assert(auth_body_bits(sequence, 0x1F6C5054, 5, 14, false) == 7359);
    assert(auth_body_bits(sequence, 0x1F6C5054, 1, 28, false) == 0);
    assert(auth_body_bits(sequence, 0x1F6C5054, 5, 12, false) == 0);
    std::array<std::byte, 1024> sequenceBody{};
    sunrise::middleware::encoding::bits::Writer sequenceWriter(sequenceBody);
    assert(write_auth_body(sequenceWriter, sequence, 0x1F6C5054, 5, 14, false));
    std::size_t sequenceBytes = 0;
    assert(sequenceWriter.finish(sequenceBytes) && sequenceBytes == 920);
    sunrise::middleware::encoding::bits::Reader sequenceReader(
        std::span<const std::byte>(sequenceBody.data(), sequenceBytes));
    assert(sequenceReader.read(64, field) && field == 7);
    assert(sequenceReader.read(64, field) && field == 0);
    assert(sequenceReader.read(8, field) && field == 1);
    assert(sequenceReader.read(32, field) && field == 7);
    assert(sequenceReader.skip(96));
    assert(sequenceReader.read(32, field) && field == 0x1F6C5054);
    assert(sequenceReader.read(7, field) && field == 59);
    assert(sequenceReader.read(16, field) && field == 32768 + 394);
    assert(sequenceReader.read(32, field) && field == 0);
    assert(sequenceReader.read(7, field) && field == 0);
    assert(sequenceReader.read(16, field) && field == 32767);

    Snapshot dropship = sequence;
    dropship.contentStep.step = ContentStep::glimmerSite0ShipSpawn;
    assert(auth_body_bits(dropship, 0x1F6C5054, 5, 13, false) == 0);
    assert(auth_body_bits(dropship, 0x1F6C5054, 5, 14, false) == 0);
    assert(auth_body_bits(dropship, 0x1F6C5054, 1, 28, false) == 278);
    std::array<std::byte, 64> dropshipBody{};
    sunrise::middleware::encoding::bits::Writer dropshipWriter(dropshipBody);
    assert(write_auth_body(dropshipWriter, dropship, 0x1F6C5054, 1, 28, false));
    std::size_t dropshipBytes = 0;
    assert(dropshipWriter.finish(dropshipBytes) && dropshipBytes == 35);
    sunrise::middleware::encoding::bits::Reader dropshipReader(
        std::span<const std::byte>(dropshipBody.data(), dropshipBytes));
    assert(dropshipReader.skip(142));
    assert(dropshipReader.read(1, field) && field == 1);
    assert(dropshipReader.read(31, field) && field == 7);
    assert(dropshipReader.skip(4));
    assert(dropshipReader.read(1, field) && field == 1);
    assert(dropshipReader.read(32, field) && field == 0x1F6C5054);
    assert(dropshipReader.read(7, field) && field == 67);
    assert(dropshipReader.read(16, field) && field == 32768 + 324);

    // Crew, completion, and chest step names are compiler-visible placeholders only. None has a
    // package-proven auth owner/body, so the outbound encoder must fail closed even when the exact
    // Glimmer group and active-sequence auth slot are present.
    const std::array<std::uint8_t, 1> contentTypes{5};
    const std::array<std::uint8_t, 1> contentFlags{kSlotAuthFlag};
    const std::array<std::uint16_t, 1> contentIndices{14};
    Snapshot contentRoster{};
    contentRoster.patchEpoch = {~std::uint64_t{}, ~std::uint64_t{}};
    contentRoster.roster.groups[0] = {
        0x1F6C5054, contentTypes, contentFlags, contentIndices};
    contentRoster.roster.groupCount = 1;
    contentRoster.roster.topLevelGroupCount = 1;
    contentRoster.roster.playerKeyGroup = 0x1F6C5054;
    contentRoster.lifetime = 3;
    contentRoster.hasContentStep = true;
    contentRoster.contentStep.step = ContentStep::glimmerSite0Enter;
    contentRoster.contentStep.generation = 8;
    std::array<std::byte, 2048> contentRosterBody{};
    std::size_t contentRosterBytes = 0;
    assert(encode_sensor_auth_update(contentRoster, contentRosterBody, contentRosterBytes));
    const std::array<ContentStep, 5> unsupportedContentSteps{
        ContentStep::glimmerSite0Crew,
        ContentStep::glimmerSite1Crew,
        ContentStep::glimmerSite2Crew,
        ContentStep::glimmerComplete,
        ContentStep::glimmerNormalChest,
    };
    for (const ContentStep unsupported : unsupportedContentSteps) {
        contentRoster.contentStep.step = unsupported;
        contentRosterBytes = 0;
        assert(auth_body_bits(contentRoster, 0x1F6C5054, 5, 14, false) == 0);
        assert(!encode_sensor_auth_update(contentRoster, contentRosterBody, contentRosterBytes));
    }

    snapshot.sense.delta = {0x220C3124, 0, 0};
    snapshot.sense.deltaWidth = 30;
    snapshot.sense.reset = true;
    std::size_t seededBytes = 0;
    assert(encode_sensor_auth_update(snapshot, contributed, seededBytes));
    assert(seededBytes == contributedBytes);

    snapshot.sense.generation = 4;
    assert(encode_sensor_auth_update(snapshot, contributed, contributedBytes));

    constexpr std::array<std::byte, 40> captured{
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0x25}, std::byte{0x30}, std::byte{0xC3}, std::byte{0x03},
        std::byte{0xA0}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0E},
        std::byte{0xF2}, std::byte{0x98}, std::byte{0x61}, std::byte{0x81},
        std::byte{0xD0}, std::byte{0x50}, std::byte{0x21}, std::byte{0xF1},
        std::byte{0x06}, std::byte{0x18}, std::byte{0x92}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x80}};
    std::uint32_t generation = 0;
    std::uint32_t delta = 0;
    assert(sunrise::middleware::bap::activity_message::sense_update::
               parse_trostland_spawner_generation(captured, generation, delta));
    assert(generation == 3);
    assert(delta == 0x220C3124);

    // Live build-86657 reports after an auth pulse repeat the same object block twice. The two
    // generations must agree; a delta wider than the bounded research value is intentionally zero.
    std::array<std::byte, 96> duplicateCapture{};
    sunrise::middleware::encoding::bits::Writer duplicateWriter(duplicateCapture);
    assert(duplicateWriter.write(~std::uint64_t{}, 64));
    assert(duplicateWriter.write(~std::uint64_t{}, 64));
    assert(duplicateWriter.write(0, 1));
    assert(duplicateWriter.write(0, 1));
    assert(duplicateWriter.write(1, 1));
    assert(duplicateWriter.write(0x2986181D, 32));
    constexpr std::uint32_t duplicateDeltaBits = 48;
    assert(duplicateWriter.write(177 + 2 * duplicateDeltaBits, 32));
    for (std::size_t object = 0; object < 2; ++object) {
        assert(duplicateWriter.write(1, 1));
        assert(duplicateWriter.write(0x2986181D, 32));
        assert(duplicateWriter.write(2, 7));
        assert(duplicateWriter.write(32768 + 271, 16));
        assert(duplicateWriter.write(0x123456789ABCULL, duplicateDeltaBits));
        assert(duplicateWriter.write(5, 32));
    }
    assert(duplicateWriter.write(0, 1));
    assert(duplicateWriter.write(0, 1));
    assert(duplicateWriter.write(0, 1));
    std::size_t duplicateBytes = 0;
    assert(duplicateWriter.finish(duplicateBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::
               parse_trostland_spawner_generation(
                   std::span<const std::byte>(duplicateCapture.data(), duplicateBytes),
                   generation,
                   delta));
    assert(generation == 5);
    assert(delta == 0);

    // The same exact duplicated grammar is used by the site-0 dropship spawner report.
    sunrise::middleware::encoding::bits::Writer dropshipSenseWriter(duplicateCapture);
    assert(dropshipSenseWriter.write(~std::uint64_t{}, 64));
    assert(dropshipSenseWriter.write(~std::uint64_t{}, 64));
    assert(dropshipSenseWriter.write(0, 1));
    assert(dropshipSenseWriter.write(0, 1));
    assert(dropshipSenseWriter.write(1, 1));
    assert(dropshipSenseWriter.write(0x2986181D, 32));
    assert(dropshipSenseWriter.write(177 + 2 * 24, 32));
    for (std::size_t object = 0; object < 2; ++object) {
        assert(dropshipSenseWriter.write(1, 1));
        assert(dropshipSenseWriter.write(0x2986181D, 32));
        assert(dropshipSenseWriter.write(2, 7));
        assert(dropshipSenseWriter.write(32768 + 220, 16));
        assert(dropshipSenseWriter.write(0x842124, 24));
        assert(dropshipSenseWriter.write(7, 32));
    }
    assert(dropshipSenseWriter.write(0, 1));
    assert(dropshipSenseWriter.write(0, 1));
    assert(dropshipSenseWriter.write(0, 1));
    std::size_t dropshipSenseBytes = 0;
    assert(dropshipSenseWriter.finish(dropshipSenseBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::
               parse_trostland_type1_generation(
                   std::span<const std::byte>(duplicateCapture.data(), dropshipSenseBytes),
                   220,
                   generation,
                   delta));
    assert(generation == 7 && delta == 0x842124);
    assert(sunrise::middleware::bap::activity_message::sense_update::
               parse_type1_generation(
                   std::span<const std::byte>(duplicateCapture.data(), dropshipSenseBytes),
                   0x2986181D,
                   220,
                   generation,
                   delta));
    assert(!sunrise::middleware::bap::activity_message::sense_update::
                parse_type1_generation(
                    std::span<const std::byte>(duplicateCapture.data(), dropshipSenseBytes),
                    0x1F6C5054,
                    220,
                    generation,
                    delta));

    // The live first-site response contains the new dropship and the existing defender twice.
    sunrise::middleware::encoding::bits::Writer mixedSenseWriter(duplicateCapture);
    assert(mixedSenseWriter.write(~std::uint64_t{}, 64));
    assert(mixedSenseWriter.write(~std::uint64_t{}, 64));
    assert(mixedSenseWriter.write(0, 1));
    assert(mixedSenseWriter.write(0, 1));
    assert(mixedSenseWriter.write(1, 1));
    assert(mixedSenseWriter.write(0x2986181D, 32));
    assert(mixedSenseWriter.write(545, 32));
    constexpr std::array<std::uint16_t, 4> mixedSlots{220, 271, 220, 271};
    for (const std::uint16_t slot : mixedSlots) {
        assert(mixedSenseWriter.write(1, 1));
        assert(mixedSenseWriter.write(0x2986181D, 32));
        assert(mixedSenseWriter.write(2, 7));
        assert(mixedSenseWriter.write(32768 + slot, 16));
        assert(mixedSenseWriter.write(slot == 220 ? 0x102480000000ULL : 0x402480000000ULL, 48));
        assert(mixedSenseWriter.write(slot == 220 ? 8 : 9, 32));
    }
    assert(mixedSenseWriter.write(0, 1));
    assert(mixedSenseWriter.write(0, 1));
    assert(mixedSenseWriter.write(0, 1));
    std::size_t mixedSenseBytes = 0;
    assert(mixedSenseWriter.finish(mixedSenseBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::
               parse_trostland_site0_spawn_generation(
                   std::span<const std::byte>(duplicateCapture.data(), mixedSenseBytes),
                   generation,
                   delta));
    assert(generation == 8 && delta == 0);

    duplicateCapture[duplicateBytes - 5] ^= std::byte{1};
    assert(!sunrise::middleware::bap::activity_message::sense_update::
                parse_trostland_spawner_generation(
                    std::span<const std::byte>(duplicateCapture.data(), duplicateBytes),
                    generation,
                    delta));

    // Fixed-width observation parsing retains generation and a correlation fingerprint only. The
    // two-object fixture matches the measured 76-bit defense-objective Sense schema.
    std::array<std::byte, 128> fixedCapture{};
    sunrise::middleware::encoding::bits::Writer fixedWriter(fixedCapture);
    assert(fixedWriter.write(~std::uint64_t{}, 64));
    assert(fixedWriter.write(~std::uint64_t{}, 64));
    assert(fixedWriter.write(0, 1));
    assert(fixedWriter.write(0, 1));
    assert(fixedWriter.write(1, 1));
    assert(fixedWriter.write(0x1F6C5054, 32));
    constexpr std::uint16_t objectiveDeltaBits = 76;
    assert(fixedWriter.write(2 * (88 + objectiveDeltaBits) + 1, 32));
    for (const std::uint16_t slot : std::array<std::uint16_t, 2>{18, 82}) {
        assert(fixedWriter.write(1, 1));
        assert(fixedWriter.write(0x1F6C5054, 32));
        assert(fixedWriter.write(4, 7));
        assert(fixedWriter.write(32768 + slot, 16));
        assert(fixedWriter.write(0xD, 4));
        assert(fixedWriter.write(0, 64));
        assert(fixedWriter.write(0, 8));
        assert(fixedWriter.write(3, 32));
    }
    assert(fixedWriter.write(0, 1));
    assert(fixedWriter.write(0, 1));
    assert(fixedWriter.write(0, 1));
    std::size_t fixedBytes = 0;
    assert(fixedWriter.finish(fixedBytes));
    sunrise::middleware::bap::activity_message::sense_update::FixedObservation observation{};
    assert(sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), fixedBytes),
        0x1F6C5054,
        4,
        82,
        objectiveDeltaBits,
        observation));
    assert(observation.generation == 3 && observation.deltaBits == objectiveDeltaBits
           && observation.matches == 1 && observation.deltaFingerprint != 0
           && observation.deltaHigh == 0xD00 && observation.deltaLow == 0);
    assert(!sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), fixedBytes),
        0x1F6C5054,
        4,
        146,
        objectiveDeltaBits,
        observation));
    assert(observation.generation == 0 && observation.matches == 0);
    assert(!sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), fixedBytes),
        0x1F6C5054,
        5,
        82,
        objectiveDeltaBits,
        observation));

    // Normal defender spawners share the variable-width type-1 schema. A single-object envelope
    // derives the exact width, while the 89-bit cap makes even two one-bit objects unambiguous.
    constexpr std::uint16_t defenderDeltaBits = 54;
    sunrise::middleware::encoding::bits::Writer defenderWriter(fixedCapture);
    assert(defenderWriter.write(~std::uint64_t{}, 64));
    assert(defenderWriter.write(~std::uint64_t{}, 64));
    assert(defenderWriter.write(0, 1));
    assert(defenderWriter.write(0, 1));
    assert(defenderWriter.write(1, 1));
    assert(defenderWriter.write(0x1F6C5054, 32));
    assert(defenderWriter.write(88 + defenderDeltaBits + 1, 32));
    assert(defenderWriter.write(1, 1));
    assert(defenderWriter.write(0x1F6C5054, 32));
    assert(defenderWriter.write(2, 7));
    assert(defenderWriter.write(32768 + 207, 16));
    assert(defenderWriter.write(0x30000000040124ULL, defenderDeltaBits));
    assert(defenderWriter.write(5, 32));
    assert(defenderWriter.write(0, 1));
    assert(defenderWriter.write(0, 1));
    assert(defenderWriter.write(0, 1));
    std::size_t defenderBytes = 0;
    assert(defenderWriter.finish(defenderBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::
               parse_single_bounded_observation(
                   std::span<const std::byte>(fixedCapture.data(), defenderBytes),
                   0x1F6C5054,
                   2,
                   207,
                   1,
                   64,
                   observation));
    assert(observation.generation == 5 && observation.matches == 1
           && observation.deltaBits == defenderDeltaBits && observation.deltaHigh == 0
           && observation.deltaLow == 0x30000000040124ULL);
    assert(!sunrise::middleware::bap::activity_message::sense_update::
                parse_single_bounded_observation(
                    std::span<const std::byte>(fixedCapture.data(), defenderBytes),
                    0x1F6C5054,
                    2,
                    207,
                    1,
                    53,
                    observation));
    assert(observation.matches == 0);

    sunrise::middleware::encoding::bits::Writer repeatedDefenderWriter(fixedCapture);
    assert(repeatedDefenderWriter.write(~std::uint64_t{}, 64));
    assert(repeatedDefenderWriter.write(~std::uint64_t{}, 64));
    assert(repeatedDefenderWriter.write(0, 1));
    assert(repeatedDefenderWriter.write(0, 1));
    assert(repeatedDefenderWriter.write(1, 1));
    assert(repeatedDefenderWriter.write(0x1F6C5054, 32));
    assert(repeatedDefenderWriter.write(2 * (88 + 1) + 1, 32));
    for (std::uint32_t generationValue : std::array<std::uint32_t, 2>{6, 6}) {
        assert(repeatedDefenderWriter.write(1, 1));
        assert(repeatedDefenderWriter.write(0x1F6C5054, 32));
        assert(repeatedDefenderWriter.write(2, 7));
        assert(repeatedDefenderWriter.write(32768 + 207, 16));
        assert(repeatedDefenderWriter.write(1, 1));
        assert(repeatedDefenderWriter.write(generationValue, 32));
    }
    assert(repeatedDefenderWriter.write(0, 1));
    assert(repeatedDefenderWriter.write(0, 1));
    assert(repeatedDefenderWriter.write(0, 1));
    std::size_t repeatedDefenderBytes = 0;
    assert(repeatedDefenderWriter.finish(repeatedDefenderBytes));
    assert(!sunrise::middleware::bap::activity_message::sense_update::
                parse_single_bounded_observation(
                    std::span<const std::byte>(fixedCapture.data(), repeatedDefenderBytes),
                    0x1F6C5054,
                    2,
                    207,
                    1,
                    89,
                    observation));
    assert(!sunrise::middleware::bap::activity_message::sense_update::
                parse_single_bounded_observation(
                    std::span<const std::byte>(fixedCapture.data(), defenderBytes),
                    0x1F6C5054,
                    2,
                    207,
                    1,
                    90,
                    observation));

    // Exact retained type-31 placement-engagement and type-71 engagement-sensor delta widths.
    constexpr std::string_view placementDelta =
        "1111000000000000000000000000000000110000000000000000000000000000000";
    static_assert(placementDelta.size() == 67);
    sunrise::middleware::encoding::bits::Writer placementWriter(fixedCapture);
    assert(placementWriter.write(~std::uint64_t{}, 64));
    assert(placementWriter.write(~std::uint64_t{}, 64));
    assert(placementWriter.write(0, 1));
    assert(placementWriter.write(0, 1));
    assert(placementWriter.write(1, 1));
    assert(placementWriter.write(0x1F6C5054, 32));
    assert(placementWriter.write(88 + placementDelta.size() + 1, 32));
    assert(placementWriter.write(1, 1));
    assert(placementWriter.write(0x1F6C5054, 32));
    assert(placementWriter.write(31, 7));
    assert(placementWriter.write(32768 + 265, 16));
    for (const char bit : placementDelta) assert(placementWriter.write(bit == '1', 1));
    assert(placementWriter.write(1, 32));
    assert(placementWriter.write(0, 1));
    assert(placementWriter.write(0, 1));
    assert(placementWriter.write(0, 1));
    std::size_t placementBytes = 0;
    assert(placementWriter.finish(placementBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), placementBytes),
        0x1F6C5054,
        31,
        265,
        static_cast<std::uint16_t>(placementDelta.size()),
        observation));
    assert(observation.generation == 1 && observation.matches == 1
           && observation.deltaHigh == 0x7
           && observation.deltaLow == 0x8000000180000000ULL);
    const std::uint64_t placementFingerprint = observation.deltaFingerprint;

    constexpr std::string_view engagementDelta =
        "10000110011110101010100011000000000001000000000001000000000001000000111000000000000001";
    static_assert(engagementDelta.size() == 86);
    sunrise::middleware::encoding::bits::Writer engagementWriter(fixedCapture);
    assert(engagementWriter.write(~std::uint64_t{}, 64));
    assert(engagementWriter.write(~std::uint64_t{}, 64));
    assert(engagementWriter.write(0, 1));
    assert(engagementWriter.write(0, 1));
    assert(engagementWriter.write(1, 1));
    assert(engagementWriter.write(0x1F6C5054, 32));
    assert(engagementWriter.write(88 + engagementDelta.size() + 1, 32));
    assert(engagementWriter.write(1, 1));
    assert(engagementWriter.write(0x1F6C5054, 32));
    assert(engagementWriter.write(71, 7));
    assert(engagementWriter.write(32768 + 225, 16));
    for (const char bit : engagementDelta) assert(engagementWriter.write(bit == '1', 1));
    assert(engagementWriter.write(2, 32));
    assert(engagementWriter.write(0, 1));
    assert(engagementWriter.write(0, 1));
    assert(engagementWriter.write(0, 1));
    std::size_t engagementBytes = 0;
    assert(engagementWriter.finish(engagementBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), engagementBytes),
        0x1F6C5054,
        71,
        225,
        static_cast<std::uint16_t>(engagementDelta.size()),
        observation));
    assert(observation.generation == 2 && observation.matches == 1
           && observation.deltaFingerprint != placementFingerprint
           && observation.deltaHigh == 0x219EAA
           && observation.deltaLow == 0x3001001001038001ULL);

    // Cross-build public-event Sense schema predicts one root bit plus one kind-2 boolean. The
    // exact build-86657 tuple must still emit this shape before it can be interpreted.
    constexpr std::uint16_t publicEventDeltaBits = 2;
    sunrise::middleware::encoding::bits::Writer publicEventWriter(fixedCapture);
    assert(publicEventWriter.write(~std::uint64_t{}, 64));
    assert(publicEventWriter.write(~std::uint64_t{}, 64));
    assert(publicEventWriter.write(0, 1));
    assert(publicEventWriter.write(0, 1));
    assert(publicEventWriter.write(1, 1));
    assert(publicEventWriter.write(0x1F6C5054, 32));
    assert(publicEventWriter.write(88 + publicEventDeltaBits + 1, 32));
    assert(publicEventWriter.write(1, 1));
    assert(publicEventWriter.write(0x1F6C5054, 32));
    assert(publicEventWriter.write(72, 7));
    assert(publicEventWriter.write(32768 + 229, 16));
    assert(publicEventWriter.write(1, 1));
    assert(publicEventWriter.write(1, 1));
    assert(publicEventWriter.write(3, 32));
    assert(publicEventWriter.write(0, 1));
    assert(publicEventWriter.write(0, 1));
    assert(publicEventWriter.write(0, 1));
    std::size_t publicEventBytes = 0;
    assert(publicEventWriter.finish(publicEventBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), publicEventBytes),
        0x1F6C5054,
        72,
        229,
        publicEventDeltaBits,
        observation));
    assert(observation.generation == 3 && observation.matches == 1
           && observation.deltaBits == publicEventDeltaBits && observation.deltaHigh == 0
           && observation.deltaLow == 3);
    assert(!sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), publicEventBytes),
        0x1F6C5054,
        72,
        228,
        publicEventDeltaBits,
        observation));

    // Cross-build hop-on schema predicts root + three 32-bit fields + one bool = 98 bits. This
    // remains an observation probe until a build-86657 body confirms the shape.
    constexpr std::uint16_t hopOnDeltaBits = 98;
    sunrise::middleware::encoding::bits::Writer hopOnWriter(fixedCapture);
    assert(hopOnWriter.write(~std::uint64_t{}, 64));
    assert(hopOnWriter.write(~std::uint64_t{}, 64));
    assert(hopOnWriter.write(0, 1));
    assert(hopOnWriter.write(0, 1));
    assert(hopOnWriter.write(1, 1));
    assert(hopOnWriter.write(0x1F6C5054, 32));
    assert(hopOnWriter.write(88 + hopOnDeltaBits + 1, 32));
    assert(hopOnWriter.write(1, 1));
    assert(hopOnWriter.write(0x1F6C5054, 32));
    assert(hopOnWriter.write(27, 7));
    assert(hopOnWriter.write(32768 + 20, 16));
    assert(hopOnWriter.write(1, 1));
    assert(hopOnWriter.write(0x80000001, 32));
    assert(hopOnWriter.write(0x80000000, 32));
    assert(hopOnWriter.write(0x80000000, 32));
    assert(hopOnWriter.write(1, 1));
    assert(hopOnWriter.write(4, 32));
    assert(hopOnWriter.write(0, 1));
    assert(hopOnWriter.write(0, 1));
    assert(hopOnWriter.write(0, 1));
    std::size_t hopOnBytes = 0;
    assert(hopOnWriter.finish(hopOnBytes));
    assert(sunrise::middleware::bap::activity_message::sense_update::parse_fixed_observation(
        std::span<const std::byte>(fixedCapture.data(), hopOnBytes),
        0x1F6C5054,
        27,
        20,
        hopOnDeltaBits,
        observation));
    assert(observation.generation == 4 && observation.matches == 1
           && observation.deltaBits == hopOnDeltaBits
           && observation.deltaHigh == 0x300000003ULL
           && observation.deltaLow == 0x0000000100000001ULL);

    snapshot.sense.slotIndex = 272;
    std::size_t rejected = 0;
    assert(!encode_sensor_auth_update(snapshot, contributed, rejected));
    assert(rejected == 0);
}
