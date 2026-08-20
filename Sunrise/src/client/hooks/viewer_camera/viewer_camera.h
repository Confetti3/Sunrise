#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::viewer::camera {

inline constexpr std::size_t kVectorLanes = 3;
inline constexpr std::size_t kMaximumPlaybackKeyframes = 64;
using Vector = std::array<float, kVectorLanes>;

struct Pose {
    Vector position{};
    Vector forward{1.0F, 0.0F, 0.0F};
    Vector up{0.0F, 0.0F, 1.0F};
    float yaw{};
    float pitch{};
    float fov{};
};

enum class Failure : std::uint8_t {
    none,
    poseSignature,
    fovSignature,
    detourAttach,
    nativePose,
    inputBindings,
    localPlayer,
    cameraTarget,
    playerChanged,
    detourDetach,
};

struct Status {
    Pose pose{};
    Failure failure{Failure::none};
    std::uint64_t generation{};
    std::uint64_t activeSession{};
    bool installed{};
    bool requested{};
    bool active{};
    bool applied{};
};

struct PlaybackKeyframe final {
    Pose pose{};
    std::array<char, 97> label{};
    float travelSeconds{1.0F};
    float dwellSeconds{};
    bool captureSnapshot{};
};

struct PlaybackPath final {
    std::array<PlaybackKeyframe, kMaximumPlaybackKeyframes> keyframes{};
    std::size_t keyframeCount{};
    std::uint64_t activitySession{};
    std::uint64_t activityRevision{};
    std::array<char, 65> name{};
    bool loop{};
};

struct PlaybackStatus final {
    float elapsedSeconds{};
    float durationSeconds{};
    std::size_t keyframeIndex{};
    bool playing{};
    bool paused{};
};

struct SnapshotCaptureRequest final {
    std::uint64_t sequence{};
    std::uint64_t cameraSession{};
    std::size_t keyframeIndex{};
    Pose pose{};
    std::array<char, 65> pathName{};
    std::array<char, 97> keyframeLabel{};
};

/** Resolves and attaches the copied camera-pose and FOV boundaries. */
[[nodiscard]] bool install() noexcept;

/** Stops Viewer Camera and detaches both copied-output boundaries. */
[[nodiscard]] bool uninstall() noexcept;

/** Polls input once after the stock camera frame. */
void poll(std::uint32_t playerIndex) noexcept;

/** Adds raw mouse motion captured by the existing Tiger Input Window subclass. */
void add_mouse_delta(long x, long y) noexcept;

/** @return True while Viewer Camera owns raw mouse motion. */
[[nodiscard]] bool captures_mouse() noexcept;

/** Requests an enter or exit transition on the next camera poll. */
void request_active(bool active) noexcept;

/** Moves the detached camera to a copied pose without changing the player. */
[[nodiscard]] bool move_to(const Pose& pose) noexcept;

/** Moves the detached camera to the latest copied player position. */
[[nodiscard]] bool return_to_player() noexcept;

/** @return True from the enter request until exit completes. */
[[nodiscard]] bool requested() noexcept;

/** @return True only while a detached pose is applied. */
[[nodiscard]] bool active() noexcept;

/** @return Nonzero identity for the current active session, or zero while inactive. */
[[nodiscard]] std::uint64_t active_session() noexcept;

/** Copies the detached pose without taking the Viewer state lock. */
[[nodiscard]] bool pose_snapshot(Pose& pose) noexcept;

/** @return One coherent copied runtime snapshot. */
[[nodiscard]] Status status() noexcept;

/** Queues a copied path for camera-thread playback. */
[[nodiscard]] bool request_playback(const PlaybackPath& path) noexcept;
void request_playback_pause(bool paused) noexcept;
void request_playback_stop() noexcept;
void request_playback_scrub(float seconds) noexcept;
[[nodiscard]] PlaybackStatus playback_status() noexcept;
[[nodiscard]] bool consume_snapshot_capture_request(SnapshotCaptureRequest& request) noexcept;

/** @return A stable label for one failure value. */
[[nodiscard]] const char* failure_name(Failure failure) noexcept;

} // namespace sunrise::client::viewer::camera
