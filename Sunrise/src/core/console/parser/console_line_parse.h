#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../definition.h"
#include "../registry/console_registry.h"

namespace sunrise::core::console::parser {

/**
 * Storage for one typed line.
 *
 * A line is an entry name and its arguments, and both are readable words rather than blobs, so
 * this sits far above anything a reader has cause to type.
 */
inline constexpr std::size_t kLineCapacity = 256;
/** Tokens one line may carry: the entry name, then one per declared argument. */
inline constexpr std::size_t kTokenCapacity = kArgumentCapacity + 1;

/** One line split into tokens that borrow from it. */
struct Tokens {
    std::array<std::string_view, kTokenCapacity> items{};
    std::size_t count{};
    /** Set when the line carried more tokens than any entry can declare. */
    bool overflowed{};
    /** Set when a quoted token was opened and never closed. */
    bool unterminatedQuote{};
    /** Set when a closing quote was followed by a non-separator byte. */
    bool malformedQuote{};
};

/**
 * Splits a line on spaces, keeping a double-quoted run as one token.
 *
 * Quoting exists because a text value may carry a space, and the alternative — refusing such a
 * value — would be a limit a reader discovers only by hitting it. The quotes are removed from the
 * token they delimit.
 *
 * @param line Whole line, already trimmed of its trailing newline.
 * @return The tokens, borrowing from the line.
 */
[[nodiscard]] constexpr Tokens tokenize(std::string_view line) noexcept {
    Tokens output{};
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && line[index] == ' ') {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }
        if (output.count >= kTokenCapacity) {
            output.overflowed = true;
            return output;
        }

        const bool quoted = line[index] == '"';
        if (quoted) {
            ++index;
        }
        const std::size_t start = index;
        while (index < line.size()) {
            if (quoted && line[index] == '\\' && index + 1 < line.size()) {
                index += 2;
                continue;
            }
            if ((quoted && line[index] == '"') || (!quoted && line[index] == ' ')) {
                break;
            }
            ++index;
        }
        if (quoted && index >= line.size()) {
            output.unterminatedQuote = true;
            return output;
        }
        output.items[output.count] = line.substr(start, index - start);
        ++output.count;
        if (quoted) {
            // A quoted token ends at its closing quote; concatenation has no documented meaning.
            ++index;
            if (index < line.size() && line[index] != ' ') {
                output.malformedQuote = true;
                return output;
            }
        }
    }
    return output;
}

/**
 * One line resolved against the registry and checked against what its entry declares.
 *
 * The descriptor is copied with the unique identity of its registration. Invocation revalidates
 * that identity immediately before calling a handler, so unregistering or replacing an entry
 * cancels already parsed and queued work instead of calling stale module state.
 */
struct Invocation {
    registry::Descriptor entry{};
    registry::RegistrationId registration{registry::kNoRegistration};
    std::array<Value, kArgumentCapacity> arguments{};
    std::size_t argumentCount{};
    /** Set when a variable line carried a value, which makes it a write rather than a read. */
    bool writesValue{};
};

/** What reading one line produced. */
struct Outcome {
    Status status{Status::ok};
    Invocation invocation{};
    /** Index of the argument that failed, meaningful only for an argument status. */
    std::size_t failedArgument{};
    /** Name the line asked for, kept so an unknown-name report can quote it. */
    std::string_view requestedName{};
};

/**
 * Reads one line into a checked invocation.
 *
 * Every check that can be made from the declaration alone is made here, so a handler receives
 * arguments it never has to re-validate: the name resolved, the count matched, each token read as
 * its declared type, and each value fell inside its declared bounds or choices.
 *
 * @param line Whole line as typed.
 * @return The outcome. The invocation is meaningful only when the status is `ok`.
 */
[[nodiscard]] Outcome parse_line(std::string_view line) noexcept;

// An empty line is not an error, it is simply nothing to run.
static_assert(tokenize("").count == 0);
static_assert(tokenize("   ").count == 0);
// Runs of spaces collapse, so a reader's stray double space is not an empty argument.
static_assert(tokenize("movement.fly_speed  20").count == 2);
static_assert(tokenize("  movement.fly_speed 20  ").items[0] == "movement.fly_speed");
static_assert(tokenize("movement.fly_speed 20").items[1] == "20");
// A quoted run stays one token and loses its quotes.
static_assert(tokenize("log.core \"two words\"").count == 2);
static_assert(tokenize("log.core \"two words\"").items[1] == "two words");
static_assert(tokenize("a \"b c\" d").count == 3);
static_assert(tokenize("a \"b c\" d").items[2] == "d");
static_assert(tokenize("a \"unclosed").unterminatedQuote);
static_assert(tokenize("a b c d e f g").overflowed);

} // namespace sunrise::core::console::parser
