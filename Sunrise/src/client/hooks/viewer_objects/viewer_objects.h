#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::viewer::objects {

inline constexpr std::size_t kObservationCapacity = 1024;

struct Observation final {
    std::uint32_t handle{};
    std::uint8_t type{};
    std::array<float, 3> position{};
    bool positionPresent{};
};

struct Snapshot final {
    std::array<Observation, kObservationCapacity> objects{};
    std::uint32_t declaredCount{};
    std::uint16_t objectCount{};
    std::uint64_t sequence{};
    bool present{};
    bool truncated{};
};

/**
 * Resolves the exact-image object iterator and object datum layout.
 * @return True when every
 * required target was resolved for the supported image.
 */
[[nodiscard]] bool install() noexcept;

/** Drops resolved native targets and copied observations. */
void uninstall() noexcept;

/**
 * Copies one physics-owned object position while the native component is live.
 * @param
 * component Transient physics component observed on the game thread.
 */
void observe_physics_component(void* component) noexcept;

/** Periodically publishes a bounded copy of every occupied object datum. */
void poll() noexcept;

/** Clears activity-scoped copied positions so recycled handles cannot inherit stale transforms. */
void reset_activity() noexcept;

/**
 * Copies the latest pointer-free object observation.
 * @param output Destination for the
 * coherent bounded snapshot.
 * @return True when at least one native iterator pass has been
 * published.
 */
[[nodiscard]] bool snapshot(Snapshot& output) noexcept;
/** @return True while all exact-image object targets remain resolved. */
[[nodiscard]] bool installed() noexcept;

} // namespace sunrise::client::viewer::objects
