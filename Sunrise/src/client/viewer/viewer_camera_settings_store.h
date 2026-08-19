#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::viewer {

inline constexpr std::uint32_t kNoKey = 0;
inline constexpr float kDefaultSpeed = 15.0F;
inline constexpr float kMinimumSpeed = 0.5F;
inline constexpr float kMaximumSpeed = 250.0F;
inline constexpr float kDefaultBoostMultiplier = 4.0F;
inline constexpr float kMinimumBoostMultiplier = 1.0F;
inline constexpr float kMaximumBoostMultiplier = 10.0F;
inline constexpr float kDefaultPrecisionMultiplier = 0.2F;
inline constexpr float kMinimumPrecisionMultiplier = 0.05F;
inline constexpr float kMaximumPrecisionMultiplier = 1.0F;
inline constexpr float kDefaultMouseSensitivity = 0.0025F;
inline constexpr float kMinimumMouseSensitivity = 0.0005F;
inline constexpr float kMaximumMouseSensitivity = 0.01F;
inline constexpr float kNativeFov = 0.0F;
inline constexpr float kMinimumFov = 20.0F;
inline constexpr float kMaximumFov = 150.0F;
inline constexpr std::size_t kBookmarkCount = 4;
inline constexpr std::size_t kVectorLanes = 3;

struct Bookmark {
    std::array<float, kVectorLanes> position{};
    float yaw{};
    float pitch{};
    float fov{kNativeFov};
    bool valid{};
};

struct Settings {
    std::uint32_t toggleKey{kNoKey};
    float speed{kDefaultSpeed};
    float boostMultiplier{kDefaultBoostMultiplier};
    float precisionMultiplier{kDefaultPrecisionMultiplier};
    float mouseSensitivity{kDefaultMouseSensitivity};
    float fov{kNativeFov};
    bool hideWeaponOnEnter{};
    bool removeHudOnEnter{};
    std::array<Bookmark, kBookmarkCount> bookmarks{};
};

/** Resolves viewer.json and loads it when one exists. */
void initialize(void* module) noexcept;

/** Drops the settings and resolved path. */
void shutdown() noexcept;

/** @return One lock-consistent settings copy. */
[[nodiscard]] Settings get() noexcept;

/** Publishes and persists one validated settings copy. */
[[nodiscard]] bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::viewer
