#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "middleware/bap/activity_message/replicate_membership.h"

namespace membership =
    sunrise::middleware::bap::activity_message::replicate_membership;

namespace {

[[nodiscard]] bool bit_at(const std::array<std::byte, membership::kEncodedSize>& bytes,
                          std::size_t bit) {
    const auto value = std::to_integer<std::uint8_t>(bytes[bit / 8]);
    return ((value >> (7U - static_cast<unsigned>(bit % 8))) & 1U) != 0;
}

} // namespace

int main() {
    membership::MembershipSnapshot snapshot{};
    snapshot.identity.memberKey = 0x0102030405060708ULL;
    snapshot.identity.field1 = -1;
    snapshot.identity.field2 = -1;
    snapshot.identity.field3 = 0x1112131415161718ULL;
    snapshot.identity.accountSoid = 0x2122232425262728ULL;
    snapshot.identity.field5 = 0x3132333435363738ULL;
    snapshot.identity.field6 = 0x4142434445464748ULL;
    snapshot.revision = 1;
    snapshot.epoch = 2;
    snapshot.transitionToken = 3;

    std::array<std::byte, membership::kEncodedSize> output{};
    std::size_t written = 0;
    assert(membership::encode_replicate_membership(snapshot, output, written));
    assert(written == 3'746);
    assert(membership::kMeaningfulBitCount == 29'968);
    assert(membership::kRegionBlockEndBit == 29'899);

    // Wire 2 names logical remote slot 1. Naming local slot 0 selects the local-ambassador path.
    assert(!bit_at(output, 879));
    assert(!bit_at(output, 880));
    assert(!bit_at(output, 881));
    assert(!bit_at(output, 882));
    assert(bit_at(output, 883));
    assert(!bit_at(output, 884));

    // The host is present and reflects the populated member key before two absent fields.
    assert(!bit_at(output, 29'831));
    assert(bit_at(output, 29'832));
    assert(!bit_at(output, 29'897));
    assert(!bit_at(output, 29'898));
    // Top-level field 4 immediately follows and is present to create the local player.
    assert(bit_at(output, membership::kRegionBlockEndBit));
}
