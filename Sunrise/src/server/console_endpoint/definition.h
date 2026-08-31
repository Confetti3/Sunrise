#pragma once

#include <cstddef>

namespace sunrise::server::console_endpoint {

/**
 * Bytes one request line may carry, the newline that frames it included.
 *
 * A request is a JSON object wrapping one console line, and a console line is itself bounded, so
 * this holds the widest line plus the envelope around it.
 *
 * What a client author has to size against, in bytes on the wire: the object text may reach 511,
 * with its newline as the 512th. `{"id":N,"line":"..."}` spends 17 bytes on the envelope plus the
 * digits of the id, so a one-digit id leaves 493 for the line and a twenty-digit one leaves 474.
 * That is the *escaped* line: a quote or a backslash costs two bytes and a control byte costs six,
 * so a line made of quotes reaches the bound at half its length. `parser::parse_line` itself
 * enforces no length limit of its own — only a token-count overflow and an unterminated quote — so
 * plain text runs all the way to this envelope bound, roughly 493 bytes for a one-digit id, before
 * anything refuses it.
 *
 * Request objects must put `id` first so later parse failures can be answered on it. A request that
 * exceeds this whole-line bound is answered on id 0 because the bytes carrying its id are discarded
 * before framing completes. Clients must retain their own request timeout. The endpoint also closes
 * an ordinary idle or partial-line connection after 30 seconds so one local peer cannot reserve its
 * single connection forever.
 */
inline constexpr std::size_t kRequestCapacity = 512;
/**
 * Bytes one response may carry.
 *
 * A response carries a summary and up to `kRowCapacity` named values, each printed as text, so
 * this is sized to hold the widest result the console can produce.
 */
inline constexpr std::size_t kResponseCapacity = 4096;
/**
 * Bytes one registry listing may carry.
 *
 * A describe answers with the whole table at once — that single call is what spares a caller a
 * request per entry — so this covers `core::console::kEntryCapacity` entries rather than the one
 * result `kResponseCapacity` is sized for, which would cut the listing off inside its first few
 * entries. An entry prints its name, its help sentence, its bounds and its choices, which is a
 * kilobyte once the JSON around them is counted, so this is 128 entries at that width. A table
 * filled to the maximum of every one of those fields at once reaches this and is reported cut;
 * entries as they are actually written come to a fifth of it.
 */
inline constexpr std::size_t kRegistryCapacity = 128 * 1024;

} // namespace sunrise::server::console_endpoint
