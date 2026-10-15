#pragma once

namespace sunrise::client::diagnostics::studio {

// These functions run only under the studio link lock.

/** Answers each editor request for one object once, on the existing camera callback. */
void poll_object_link() noexcept;
/** Closes the mapping when the core connection drops. */
void drop_object_link() noexcept;
/** Releases the mapping; call after frame publication has stopped. */
void close_object_link() noexcept;

} // namespace sunrise::client::diagnostics::studio
