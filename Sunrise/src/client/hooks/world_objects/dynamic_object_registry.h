#pragma once

#include "world_object_registry.h"

namespace sunrise::client::hooks::world_objects {

struct DynamicInstance final {
    Instance identity{};
    std::uint32_t classTag{};
};

/** Captures the native class selected before a successful dynamic object's construction. */
void observe_dynamic(std::uint32_t handle, const void* entry) noexcept;
/** Erases the exact full handle before the native object slot is recycled. */
void erase_dynamic(std::uint32_t handle) noexcept;
/** Reset only at hook installation or after all lifetime callbacks have stopped. */
void reset_dynamic_registry(bool available) noexcept;
/** Copies candidates without waiting for a native lifetime writer. */
[[nodiscard]] SnapshotResult try_dynamic_snapshot(std::span<DynamicInstance> output) noexcept;

} // namespace sunrise::client::hooks::world_objects
