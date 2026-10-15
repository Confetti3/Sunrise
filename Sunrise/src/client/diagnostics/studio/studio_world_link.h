#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../hooks/world_objects/world_object_registry.h"

namespace sunrise::client::diagnostics::studio {

/** One row of the last published scan, as the object link needs it. */
struct PublishedObject final {
    hooks::world_objects::Instance identity{};
    std::uint32_t classTag{};
    bool dynamic{};
};

enum class Lookup : std::uint8_t { found, absent, unavailable };

// These functions run only under the studio link lock.

/** Resolve native readers during hook installation, before the frame callback starts. */
[[nodiscard]] bool initialize_world_link() noexcept;
/** Called only while the editor's core diagnostic connection is active. */
void poll_world_link() noexcept;
/** Closes the mapping when the core connection drops; readers stay resolved. */
void drop_world_link() noexcept;
/** Close after frame publication has stopped. */
void close_world_link() noexcept;
/** Full handle of the object the local player controls, or all bits set for none. */
void set_world_player(std::uint32_t handle) noexcept;

/**
 * Finds one row of the last published scan and the rows attached to it.
 * @param output Receives the row's identity, class and lane.
 * @param children Receives handle and generation pairs of attached rows.
 * @param childCount Receives the number of attached rows, which can exceed children.size().
 * @return absent when the scan holds no such row; unavailable before any publication.
 */
[[nodiscard]] Lookup find_published_object(std::uint32_t handle,
                                           std::uint32_t generation,
                                           PublishedObject& output,
                                           std::span<std::array<std::uint32_t, 2>> children,
                                           std::size_t& childCount) noexcept;

} // namespace sunrise::client::diagnostics::studio
