#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sunrise::client::viewer::paths {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::size_t kMaximumPathCount = 32;
inline constexpr std::size_t kMaximumKeyframeCount = 64;
inline constexpr std::size_t kMaximumPathNameBytes = 64;
inline constexpr std::size_t kMaximumKeyframeLabelBytes = 96;
inline constexpr float kMinimumKeyframeFov = 20.0F;
inline constexpr float kMaximumKeyframeFov = 150.0F;
inline constexpr float kMaximumPitch = 1.55334303427495F;
inline constexpr float kMaximumSegmentSeconds = 3600.0F;

/** Stable, pointer-free identity copied from an inspection node when a keyframe is recorded. */
struct SelectionIdentity final {
    std::uint64_t producerEpoch{};
    std::uint64_t nativeKey{};
    std::uint32_t producer{};
    std::uint32_t kind{};

    [[nodiscard]] friend bool operator==(const SelectionIdentity&,
                                         const SelectionIdentity&) noexcept = default;
};

struct Keyframe final {
    std::array<float, 3> position{};
    float yaw{};
    float pitch{};
    float fov{kMinimumKeyframeFov};
    /** Travel time from this keyframe to the following keyframe. */
    float travelSeconds{1.0F};
    /** Time held after this keyframe is reached. */
    float dwellSeconds{};
    std::string label;
    std::optional<SelectionIdentity> selection;
    bool captureSnapshot{};

    [[nodiscard]] friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

struct CameraPath final {
    std::string name;
    std::vector<Keyframe> keyframes;
    bool loop{};

    [[nodiscard]] friend bool operator==(const CameraPath&, const CameraPath&) = default;
};

struct Library final {
    std::uint32_t schemaVersion{kSchemaVersion};
    std::vector<CameraPath> paths;

    [[nodiscard]] friend bool operator==(const Library&, const Library&) = default;
};

/** Resolves viewer-paths.json and loads it when one exists. */
void initialize(void* module) noexcept;

/** Drops every copied path and the resolved persistence path. */
void shutdown() noexcept;

/** @return One lock-consistent, pointer-free path-library copy. */
[[nodiscard]] Library get();

/** @return True when the complete library fits the persisted schema and runtime bounds. */
[[nodiscard]] bool valid(const Library& library) noexcept;

/** Atomically persists and then publishes one validated path-library copy. */
[[nodiscard]] bool publish(const Library& library) noexcept;

} // namespace sunrise::client::viewer::paths
