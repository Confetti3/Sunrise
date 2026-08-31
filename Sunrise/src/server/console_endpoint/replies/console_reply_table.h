#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../core/console/definition.h"

namespace sunrise::server::console_endpoint::replies {

/**
 * Results waiting to be written back.
 *
 * One caller sends a handful of lines before reading, so the depth only has to absorb that burst.
 * Past it the oldest is dropped: a caller that stopped reading must not grow this without bound.
 */
inline constexpr std::size_t kReplyCapacity = 32;

/**
 * Stores one finished result under its ticket.
 * @param ticket Ticket the invocation was submitted with. Zero is never stored.
 * @param result What the handler reported.
 * @return True when it was stored. False only for a zero ticket.
 */
bool remember(std::uint64_t ticket, const core::console::Result& result) noexcept;

/**
 * Takes one result back, removing it.
 * @param ticket Ticket to look for.
 * @param output Filled only when the ticket was found.
 * @return True when a result was found and removed.
 */
[[nodiscard]] bool take(std::uint64_t ticket, core::console::Result& output) noexcept;

/** Drops every stored result. */
void clear() noexcept;

} // namespace sunrise::server::console_endpoint::replies
