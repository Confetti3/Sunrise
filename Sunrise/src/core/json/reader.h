#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::core::json {

/**
 * Fixed-storage JSON token reader. It never allocates. Callers build their own object parsers on
 * it and refuse the whole input on the first false return.
 */
class Reader {
public:
    /** @param input Complete JSON text, borrowed and never changed. */
    explicit Reader(std::string_view input) noexcept;

    /** Checks and skips one value of any type. @param depth Current nesting depth. */
    [[nodiscard]] bool skip_value(unsigned depth) noexcept;
    /** Reads one string. @param output Receives the borrowed bytes between the quotes. */
    [[nodiscard]] bool string(std::string_view& output) noexcept;
    /** Reads a true or false literal. */
    [[nodiscard]] bool boolean(bool& output) noexcept;
    /** Reads one unsigned decimal integer with no precision loss. */
    [[nodiscard]] bool unsigned_integer(std::uint64_t& output) noexcept;
    /** Reads an unsigned id as an integer or a quoted hex token. */
    [[nodiscard]] bool unsigned_value(std::uint64_t& output) noexcept;
    /** Reads one signed integer. Fractions and exponents are refused. */
    [[nodiscard]] bool signed_integer(std::int64_t& output) noexcept;
    /** Reads one signed integer that has to fit a native signed byte. */
    [[nodiscard]] bool signed_byte(std::int8_t& output) noexcept;
    /** Reads one signed integer that has to fit a native signed 32-bit value. */
    [[nodiscard]] bool signed_32(std::int32_t& output) noexcept;
    /** Reads one finite number that has to fit a float. */
    [[nodiscard]] bool floating_point(float& output) noexcept;
    /** Reads one complete number. */
    [[nodiscard]] bool number() noexcept;
    /** Reads one exact literal after leading whitespace. @param value Literal bytes to match. */
    [[nodiscard]] bool literal(std::string_view value) noexcept;
    /** Reads one expected structural character after whitespace. */
    [[nodiscard]] bool consume(char value) noexcept;
    /** @return True when only trailing whitespace remains. */
    [[nodiscard]] bool at_end() noexcept;
    /** Skips JSON whitespace. Other control bytes are left alone. */
    void whitespace() noexcept;
    /** @return Byte offset of the next unread character, for error reports. */
    [[nodiscard]] std::size_t position() const noexcept;

private:
    std::string_view input_;
    std::size_t position_{};
};

/**
 * Decodes the escapes in one raw JSON string into UTF-8.
 * @param raw Bytes between the quotes, as `Reader::string` returns them.
 * @param output Destination storage. It is not null-terminated.
 * @param length Receives the decoded byte count.
 * @return False when the text does not fit or holds an unpaired surrogate.
 */
[[nodiscard]] bool
decode_string(std::string_view raw, std::span<char> output, std::size_t& length) noexcept;

} // namespace sunrise::core::json
