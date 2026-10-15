#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::diagnostics::studio {

// These functions run only under the studio link lock.

/** The observer preserves native calls and copies completed primary poses while Studio asks. */
[[nodiscard]] bool initialize_pose_link() noexcept;
/** Only the existing camera callback polls the editor-owned mapping. */
void poll_pose_link() noexcept;
/** Ends any capture window and closes the mapping when the core connection drops. */
void drop_pose_link() noexcept;
/** A world transition drops the capture in progress and the published one. */
void set_pose_world(bool present, bool changed) noexcept;
/**
 * Lists the distinct models drawn for one exact owner lifetime in the last capture.
 * @param models Receives up to models.size() model tags.
 * @param time Receives the capture time, or 0 when no draw matches this owner.
 * @return Number of distinct models, which can exceed models.size().
 */
[[nodiscard]] std::size_t published_models(std::uint32_t owner,
                                           std::uint32_t generation,
                                           std::span<std::uint32_t> models,
                                           std::uint64_t& time) noexcept;
/** Stops capture; false keeps shutdown pending until the observer can detach. */
[[nodiscard]] bool close_pose_link() noexcept;

} // namespace sunrise::client::diagnostics::studio
