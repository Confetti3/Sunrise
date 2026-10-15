#pragma once

#include "../definition.h"

namespace sunrise::state::mods::problem {

/**
 * Records one refusal and keeps the mod out.
 * @param record Mod the refusal is about.
 * @param reason Machine key for the log.
 * @param format printf-style readable message; `%s` arguments must be null-terminated.
 */
void refuse(Record& record, Reason reason, const char* format, ...) noexcept;

/**
 * Records one warning. The mod still loads.
 * @param record Mod the warning is about.
 * @param reason Machine key for the log.
 * @param format printf-style readable message; `%s` arguments must be null-terminated.
 */
void warn(Record& record, Reason reason, const char* format, ...) noexcept;

/** @return The stable lowercase log key for one reason. */
[[nodiscard]] const char* reason_name(Reason reason) noexcept;

/** @return The first refusal recorded, or null when the mod loads. */
[[nodiscard]] const Problem* first_refusal(const Record& record) noexcept;

} // namespace sunrise::state::mods::problem
