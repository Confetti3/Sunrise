#include "console_line_parse.h"

#include "../registry/console_registry.h"
#include "console_value_parse.h"

namespace sunrise::core::console::parser {
namespace {

/**
 * Reads one token as a declared domain and applies that declaration's bounds and choices.
 * @param token Whole token.
 * @param type Declared domain.
 * @param minimum Inclusive low bound, or a value not below the high bound when unbounded.
 * @param maximum Inclusive high bound.
 * @param choices Accepted words, or empty when any value of the domain is allowed.
 * @param output Filled only when every check passes.
 * @return The first failing status, or `ok`.
 */
[[nodiscard]] Status checked_value(std::string_view token,
                                   Type type,
                                   double minimum,
                                   double maximum,
                                   std::span<const std::string_view> choices,
                                   Value& output) noexcept {
    const Status read = parse_value(token, type, output);
    if (read != Status::ok) {
        return read;
    }
    const Status bounded = check_bounds(output, minimum, maximum);
    if (bounded != Status::ok) {
        return bounded;
    }
    return check_choices(output, choices);
}

/** Reads the tail of a variable line, which is either nothing or the one value to write. */
[[nodiscard]] Outcome variable_outcome(const registry::Descriptor& entry,
                                       const Tokens& tokens) noexcept {
    Outcome outcome{};
    outcome.invocation.entry = entry;
    if (tokens.count == 1) {
        // A bare name reads the value. It is the most typed line on this surface, so it costs
        // nothing beyond the lookup that already happened.
        return outcome;
    }
    if (tokens.count > 2) {
        outcome.status = Status::wrongArgumentCount;
        return outcome;
    }

    const Status checked = checked_value(tokens.items[1],
                                         entry.type,
                                         entry.minimum,
                                         entry.maximum,
                                         entry.choices,
                                         outcome.invocation.arguments[0]);
    if (checked != Status::ok) {
        outcome.status = checked;
        outcome.failedArgument = 0;
        return outcome;
    }
    outcome.invocation.argumentCount = 1;
    outcome.invocation.writesValue = true;
    return outcome;
}

/** Reads the tail of a command line, which is one token per declared argument. */
[[nodiscard]] Outcome command_outcome(const registry::Descriptor& entry,
                                      const Tokens& tokens) noexcept {
    Outcome outcome{};
    outcome.invocation.entry = entry;
    const std::size_t supplied = tokens.count - 1;
    if (supplied != entry.arguments.size()) {
        outcome.status = Status::wrongArgumentCount;
        return outcome;
    }

    for (std::size_t index = 0; index < supplied; ++index) {
        const registry::Argument& declared = entry.arguments[index];
        const Status checked = checked_value(tokens.items[index + 1],
                                             declared.type,
                                             declared.minimum,
                                             declared.maximum,
                                             declared.choices,
                                             outcome.invocation.arguments[index]);
        if (checked != Status::ok) {
            outcome.status = checked;
            outcome.failedArgument = index;
            return outcome;
        }
    }
    outcome.invocation.argumentCount = supplied;
    return outcome;
}

} // namespace

/** Reads one line into a checked invocation. */
Outcome parse_line(std::string_view line) noexcept {
    const Tokens tokens = tokenize(line);
    if (tokens.unterminatedQuote || tokens.malformedQuote) {
        Outcome outcome{};
        outcome.status = Status::badArgument;
        return outcome;
    }
    if (tokens.overflowed) {
        Outcome outcome{};
        outcome.status = Status::wrongArgumentCount;
        return outcome;
    }
    if (tokens.count == 0) {
        // The console skips blank input before reaching here, so an empty line at this point is a
        // caller mistake and is reported as the miss it is rather than as a silent success.
        Outcome outcome{};
        outcome.status = Status::unknownName;
        return outcome;
    }

    registry::Descriptor entry{};
    registry::RegistrationId registration = registry::kNoRegistration;
    if (!registry::resolve(tokens.items[0], entry, registration)) {
        Outcome outcome{};
        outcome.status = Status::unknownName;
        outcome.requestedName = tokens.items[0];
        return outcome;
    }

    Outcome outcome = entry.kind == registry::Kind::variable ? variable_outcome(entry, tokens)
                                                             : command_outcome(entry, tokens);
    outcome.invocation.registration = registration;
    outcome.requestedName = tokens.items[0];
    return outcome;
}

} // namespace sunrise::core::console::parser
