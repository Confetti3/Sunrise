/**
 * Opcode 601 is a loot pickup. The Client sends it when it cannot pick the loot up on its own.
 * The reply is 165 bits and no field can be left out, so the bare echo under-runs the decoder.
 * The reply's tail is the request's loot identity, echoed unchanged.
 */

#include "opcode601_codec.h"

#include <array>

#include "../../../encoding/bit_reader.h"
#include "../../status_fields.h"

namespace sunrise::middleware::web_service::messages::opcode601 {
namespace {

/** The pickup kind is a 5-bit value biased by one. */
constexpr std::uint8_t kPickupKindWidth = 5;
/** The loot identity is a 32-bit part, a 64-bit part and a 32-bit auxiliary, in that order. */
constexpr std::uint8_t kIdentityPartAWidth = 32;
constexpr std::uint8_t kIdentityPartBWidth = 64;
constexpr std::uint8_t kIdentityAuxWidth = 32;
/** A payload is whole bytes, so at most this many bits of it can be trailing pad. */
constexpr std::size_t kBitsPerByte = 8;

/** Wire values of the loot identity. Request and reply share one bias, so raw bits echo. */
struct LootIdentity {
    std::uint64_t partA{};
    std::uint64_t partB{};
    std::uint64_t aux{};
};

/**
 * Reads the complete 133-bit request.
 * @param message Opcode-601 request.
 * @param identity Receives the loot identity's wire values.
 * @return True when the opcode matches and the body is exactly one request.
 */
[[nodiscard]] bool read_request(const Message& message, LootIdentity& identity) noexcept {
    identity = {};
    if (message.opcode != kOpcode) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t pickupKind = 0;
    return reader.read(kPickupKindWidth, pickupKind)
           && reader.read(kIdentityPartAWidth, identity.partA)
           && reader.read(kIdentityPartBWidth, identity.partB)
           && reader.read(kIdentityAuxWidth, identity.aux)
           && reader.remaining_bits() < kBitsPerByte;
}

} // namespace

/** Reports whether this request is one complete remote loot pickup. */
bool parse_request(const Message& message) noexcept {
    LootIdentity identity{};
    return read_request(message, identity);
}

/** Encodes the status pair and the echoed loot identity in descriptor order. */
bool encode_response(const Message& message,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    LootIdentity identity{};
    if (!read_request(message, identity)) {
        return false;
    }

    std::array<std::byte, kResponseSize> staged{};
    encoding::bits::Writer writer = begin_response(message, staged);
    // The status value is the Family-4 version the Client waits for. This route publishes no
    // revision, so it carries the no-publication value and the Client's wait completes at once.
    StatusResponse status{};
    status.value = kNoFamily4Publication;
    const bool encoded = status::write_fields(writer, ResponseShape::statusPair, status)
                         && writer.write(identity.partA, kIdentityPartAWidth)
                         && writer.write(identity.partB, kIdentityPartBWidth)
                         && writer.write(identity.aux, kIdentityAuxWidth);
    return finish_response(writer, encoded, staged, output, written);
}

} // namespace sunrise::middleware::web_service::messages::opcode601
