#pragma once

namespace sunrise::core::console::overlay {

/**
 * Publishes the console's own entries and clears its editing state.
 * @return True when the console is ready to draw.
 */
[[nodiscard]] bool initialize() noexcept;

/**
 * Runs whatever is waiting, then draws the console when it is showing.
 *
 * The queue is drained whether or not the console is showing. Work reaches it from callers that
 * are not the reader, and those must not be held until someone happens to open a window.
 *
 * @param visible Whether the console is showing.
 * @return True when draw data was built and the caller must submit the frame.
 */
[[nodiscard]] bool render(bool visible) noexcept;

/** Removes the console's entries and clears its editing state. */
void shutdown() noexcept;

} // namespace sunrise::core::console::overlay
