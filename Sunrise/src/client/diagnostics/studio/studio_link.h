#pragma once

namespace sunrise::client::diagnostics::studio {

/** Resolve diagnostic readers before accepting an editor connection. */
void initialize() noexcept;

/** Publishes diagnostics only while an editor owns a fresh local connection. */
void poll() noexcept;

/** Stops publication; false keeps shutdown pending until all observers detach. */
[[nodiscard]] bool close() noexcept;

} // namespace sunrise::client::diagnostics::studio
