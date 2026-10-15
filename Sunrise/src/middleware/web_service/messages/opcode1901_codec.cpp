#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode1901.h"

namespace sunrise::middleware::web_service::messages::opcode1901 {
namespace {

/** The native replacement array reserves twelve entries, so its count uses four bits. */
constexpr std::uint8_t kReplacementCountWidth = 4;
/** A payload is whole bytes, so at most this many bits of it can be trailing pad. */
constexpr std::size_t kBitsPerByte = 8;
/** The plug definition index is a signed 16-bit value biased from INT16_MIN. */
constexpr std::uint8_t kDefinitionIndexWidth = 16;
/** The canonical socket-kind byte is signed and biased from INT8_MIN. */
constexpr std::uint8_t kCanonicalSocketKindWidth = 8;
/** The compact model socket-kind discriminator occupies two bits. */
constexpr std::uint8_t kModelSocketKindWidth = 2;
/** The signed socket index is biased from INT32_MIN. */
constexpr std::uint8_t kSocketIndexWidth = 32;
/** The auxiliary identity and the target instance identity are unbiased 64-bit values. */
constexpr std::uint8_t kIdentityWidth = 64;
/** Nonnegative signed 16-bit values have this bit set after native descriptor biasing. */
constexpr std::uint64_t kDefinitionIndexBias = 0x8000ULL;
/** Nonnegative signed 8-bit values have this bit set after native descriptor biasing. */
constexpr std::uint64_t kCanonicalSocketKindBias = 0x80ULL;
/** Nonnegative signed 32-bit values have this bit set after native descriptor biasing. */
constexpr std::uint64_t kSocketIndexBias = 0x80000000ULL;
/** Model socket-kind zero is encoded as one so wire zero remains the absent sentinel. */
constexpr std::uint64_t kModelSocketKindBias = 1;
/** Shader Apply targets an ordinary item socket model. */
constexpr std::uint64_t kShaderModelSocketKind = 0;

} // namespace

/** Parses the complete native equipped socket-action descriptor, batch or single. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode) {
        return false;
    }

    encoding::bits::Reader reader(message.payload);
    std::uint64_t replacementCount = 0;
    if (!reader.read(kReplacementCountWidth, replacementCount) || replacementCount == 0
        || replacementCount > kReplacementCapacity) {
        return false;
    }

    // The run is counted rather than fixed, so its width follows the count. Reading exactly that
    // many and then requiring the payload to be spent is what proves the descriptor's length.
    for (std::uint64_t index = 0; index < replacementCount; ++index) {
        std::uint64_t encodedPlugDefinition = 0;
        std::uint64_t encodedCanonicalSocketKind = 0;
        std::uint64_t modelSocketKind = 0;
        std::uint64_t encodedSocketIndex = 0;
        std::uint64_t auxiliary = 0;
        if (!reader.read(kDefinitionIndexWidth, encodedPlugDefinition)
            || !reader.read(kCanonicalSocketKindWidth, encodedCanonicalSocketKind)
            || !reader.read(kModelSocketKindWidth, modelSocketKind)
            || !reader.read(kSocketIndexWidth, encodedSocketIndex)
            || !reader.read(kIdentityWidth, auxiliary)
            || encodedPlugDefinition < kDefinitionIndexBias
            || encodedCanonicalSocketKind < kCanonicalSocketKindBias
            || modelSocketKind < kModelSocketKindBias || encodedSocketIndex < kSocketIndexBias
            || modelSocketKind - kModelSocketKindBias != kShaderModelSocketKind
            || auxiliary != kShaderAuxiliary) {
            request = {};
            return false;
        }
        Replacement& replacement = request.replacements[static_cast<std::size_t>(index)];
        replacement.plugDefinitionIndex =
            static_cast<std::uint16_t>(encodedPlugDefinition - kDefinitionIndexBias);
        replacement.canonicalSocketKind =
            static_cast<std::uint8_t>(encodedCanonicalSocketKind - kCanonicalSocketKindBias);
        replacement.modelSocketKind =
            static_cast<std::uint8_t>(modelSocketKind - kModelSocketKindBias);
        replacement.socketIndex = static_cast<std::uint32_t>(encodedSocketIndex - kSocketIndexBias);
        replacement.auxiliary = auxiliary;
        if (replacement.canonicalSocketKind != replacement.socketIndex) {
            request = {};
            return false;
        }
    }
    request.replacementCount = static_cast<std::size_t>(replacementCount);

    // 4 + 122n + 64 bits; the payload pads to a whole byte, so fewer than 8 bits remain.
    if (!reader.read(kIdentityWidth, request.targetInstanceSoid)
        || reader.remaining_bits() >= kBitsPerByte || request.targetInstanceSoid == 0) {
        request = {};
        return false;
    }
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode1901
