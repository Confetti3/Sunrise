#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "middleware/web_service/messages/opcode406.h"

extern int failures;

namespace {

namespace opcode406 = sunrise::middleware::web_service::messages::opcode406;

void expect(bool value, const char* label) noexcept {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}

void write_bits(std::span<std::byte> output,
                std::size_t& position,
                std::uint64_t value,
                std::uint8_t width) noexcept {
    for (std::uint8_t index = 0; index < width; ++index) {
        const unsigned shift = static_cast<unsigned>(width - index - 1);
        const auto bit = static_cast<unsigned>((value >> shift) & 1U);
        const std::size_t byteIndex = position / 8;
        const unsigned byteShift = static_cast<unsigned>(7 - (position % 8));
        output[byteIndex] |= std::byte{static_cast<unsigned char>(bit << byteShift)};
        ++position;
    }
}

std::array<std::byte, 15> payload(std::uint32_t flags) noexcept {
    std::array<std::byte, 15> output{};
    std::size_t position = 0;
    write_bits(output, position, 1, 1);
    write_bits(output, position, 0x1234U, 64);
    write_bits(output, position, 1, 1);
    write_bits(output, position, 42, 15);
    write_bits(output, position, 0x80000000ULL + flags, 32);
    write_bits(output, position, 0, 7);
    return output;
}

} // namespace

void test_opcode406_item_state() noexcept {
    for (std::uint32_t flags = 0; flags <= 7; ++flags) {
        const auto bytes = payload(flags);
        const sunrise::middleware::web_service::Message message{
            opcode406::kOpcode, 1, bytes};
        opcode406::Request request{};
        expect(opcode406::parse_request(message, request),
               "opcode 406 accepts known item-state bits");
        expect(request.instanceSoid == 0x1234U && request.definitionIndex == 42
                   && request.flags == flags,
               "opcode 406 preserves known item-state bits");
    }

    const auto bytes = payload(8);
    const sunrise::middleware::web_service::Message message{opcode406::kOpcode, 1, bytes};
    opcode406::Request request{};
    expect(!opcode406::parse_request(message, request),
           "opcode 406 rejects unknown item-state bits");
}
