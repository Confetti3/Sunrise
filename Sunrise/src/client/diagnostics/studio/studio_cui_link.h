#pragma once

#include <cstdint>

namespace sunrise::client::diagnostics::studio {

/** Installs the UI frame observer; false leaves the CUI mirror detached. */
[[nodiscard]] bool initialize_cui_link() noexcept;
/** Stops capture and detaches; false keeps shutdown pending until the observer is idle. */
[[nodiscard]] bool close_cui_link() noexcept;

// The CUI lock owns everything below; the observer is its only caller during a frame.

/** Lets later frames connect to Studio. */
void start_cui_mapping() noexcept;
/**
 * Runs one capture once the capture interval has passed.
 * Skips the frame when another call holds the CUI lock.
 * @return True when a capture ran with Studio attached.
 */
[[nodiscard]] bool observe_cui_frame(std::uint64_t now) noexcept;
/** Waits for a capture in progress, then closes the mapping and refuses new captures. */
void close_cui_mapping() noexcept;

} // namespace sunrise::client::diagnostics::studio
