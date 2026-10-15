#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode1901 {

/** Web Service opcode used by the equipped-item shader application action. */
inline constexpr std::uint16_t kOpcode = 1901;

/** The native replacement array reserves twelve entries, which its 4-bit count can address. */
inline constexpr std::size_t kReplacementCapacity = 12;

/** Collection-backed shader sources send INT64_MIN as their auxiliary identity. */
inline constexpr std::uint64_t kShaderAuxiliary = 0x8000000000000000ULL;

/** One socket replacement: which plug goes into which socket of the named item. */
struct Replacement {
    std::uint16_t plugDefinitionIndex{};
    std::uint8_t canonicalSocketKind{};
    std::uint8_t modelSocketKind{};
    std::uint32_t socketIndex{};
    std::uint64_t auxiliary{};
};

/**
 * Exact logical fields carried by the native equipped socket-action descriptor.
 * The descriptor is a counted run of replacements followed by the one item they apply to, so its
 * width grows with the count rather than being fixed.
 */
struct Request {
    std::array<Replacement, kReplacementCapacity> replacements{};
    std::size_t replacementCount{};
    /** Whole item-instance identity of the target, sent unbiased. */
    std::uint64_t targetInstanceSoid{};
};

/**
 * Parses the exact reflected opcode-1901 descriptor.
 * The request is a bounded replacement array followed by the target's whole instance identity.
 * @param message Parsed Web Service envelope.
 * @param request Receives the plug, socket descriptor, auxiliary identity, and target instance.
 * @return True for a complete shader request of one to twelve replacements.
 */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode1901
