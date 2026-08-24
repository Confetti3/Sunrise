#pragma once

namespace sunrise::client::content::statics {

/** Starts the one-shot statics structure probe unless one already ran. */
void request_structure_probe() noexcept;

/** @return True while the probe pass is executing. */
[[nodiscard]] bool structure_probe_running() noexcept;

/** @return True once a probe pass finished, successfully or not. */
[[nodiscard]] bool structure_probe_finished() noexcept;

} // namespace sunrise::client::content::statics
