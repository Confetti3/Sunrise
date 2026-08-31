#pragma once

#include "../definition.h"
#include "../parser/console_line_parse.h"

namespace sunrise::core::console::invoke {

/**
 * Runs one checked invocation and fills its result.
 *
 * The parser has already established that the entry exists, that the argument count matches, and
 * that every value is of its declared type and inside its declared range. What is left here is
 * only the call itself and the shape of what comes back, which is why one function serves a
 * variable read, a variable write and a command alike.
 *
 * A variable answers with a row keyed by its own name, so reading `movement.fly_speed` and asking
 * a command for the same number produce the same shape of answer.
 *
 * @param invocation Checked invocation, as `parse_line` produced it.
 * @param output Result to fill. It arrives cleared.
 */
void run(const parser::Invocation& invocation, Result& output) noexcept;

} // namespace sunrise::core::console::invoke
