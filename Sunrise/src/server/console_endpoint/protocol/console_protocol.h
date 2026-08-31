#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../core/console/definition.h"
#include "../definition.h"

namespace sunrise::server::console_endpoint::protocol {

/**
 * Smallest buffer either encoder will write into.
 *
 * Below this there is no well-formed object to write, so both write nothing and report a zero
 * length rather than a fragment a caller would fail to parse and could not correlate back to what
 * it asked. Published rather than kept private so a caller sizing its own buffer can assert itself
 * above this at compile time, instead of discovering the silence the first time a result is due.
 */
inline constexpr std::size_t kMinimumCapacity = 128;

/** One decoded request. Exactly one of a line or a describe is meaningful. */
struct Request {
    /** Correlation id the caller chose. Zero is never valid, so it doubles as "absent". */
    std::uint64_t id{};
    std::array<char, kRequestCapacity> line{};
    std::size_t lineLength{};
    /** Set when the caller asked for the registry rather than for a line to run. */
    bool describe{};
};

/**
 * Reads one request object.
 *
 * The only producer is our own client, so this accepts exactly the three fields it sends and
 * refuses everything else. A malformed object is the caller's mistake, not something to guess at.
 *
 * @param text One whole line, without its terminator.
 * @param output Filled only on success, with one exception: `output.id`. When the object carried a
 * readable id before whatever else went wrong, that id is written even on failure, so the refusal
 * can be answered on it rather than on zero — nothing here times out, and a caller that indexes
 * replies by id would otherwise wait for that one forever. It stays 0 when no id could be recovered
 * at all, which is the only case left un-answerable. Nothing else in `output` is meaningful when
 * this returns false.
 * @return True when the object carried an id and either a line or a describe. False means the
 * request must not be acted on, whether or not an id came back with it.
 */
[[nodiscard]] bool decode_request(std::string_view text, Request& output) noexcept;

/**
 * Writes one result as a response object.
 *
 * The status is written as its name rather than its ordinal, so a caller never depends on the
 * order of an enumeration it cannot see. The object holds no newline, which is what frames it.
 *
 * @param id Correlation id to echo back.
 * @param result Result to report.
 * @param buffer Destination. Only the first `length` bytes are written and the rest of the span is
 * left as it was, so a reused buffer still holds the tail of whatever it carried before. Send it by
 * `length` and never by a terminator, which is not written.
 * @param length Receives the written length. Zero means nothing was written and nothing should be
 * sent: the span was too small to hold even an empty response.
 */
void encode_result(std::uint64_t id,
                   const core::console::Result& result,
                   std::span<char> buffer,
                   std::size_t& length) noexcept;

/**
 * Writes the whole registry as a response object.
 * @param id Correlation id to echo back.
 * @param buffer Destination. Only the first `length` bytes are written and the rest of the span is
 * left as it was, so a reused buffer still holds the tail of whatever it carried before. Send it by
 * `length` and never by a terminator, which is not written.
 * @param length Receives the written length. Zero means nothing was written and nothing should be
 * sent: the span was too small to hold even an empty response.
 */
void encode_registry(std::uint64_t id, std::span<char> buffer, std::size_t& length) noexcept;

/** @param status Outcome to name. @return Its wire name, as the response carries it. */
[[nodiscard]] constexpr std::string_view status_name(core::console::Status status) noexcept {
    switch (status) {
    case core::console::Status::ok:
        return "ok";
    case core::console::Status::unknownName:
        return "unknownName";
    case core::console::Status::wrongArgumentCount:
        return "wrongArgumentCount";
    case core::console::Status::badArgument:
        return "badArgument";
    case core::console::Status::outOfRange:
        return "outOfRange";
    case core::console::Status::refused:
        return "refused";
    case core::console::Status::failed:
        return "failed";
    }
    return "failed";
}

// Every outcome has to name itself, or a caller would branch on an empty string.
static_assert(!status_name(core::console::Status::ok).empty());
static_assert(!status_name(core::console::Status::outOfRange).empty());

} // namespace sunrise::server::console_endpoint::protocol
