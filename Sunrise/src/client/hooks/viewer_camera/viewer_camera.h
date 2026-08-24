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

enum class Phase : std::uint8_t {
    inactive,
    entering,
    active,
    exiting,
    faulted,
};

enum class PoseSource : std::uint8_t {
    unavailable,
    player,
    viewer,
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
    unsafePosition,
    detourDetach,
};

struct Status {
    Pose pose{};
    Failure failure{Failure::none};
    Phase phase{Phase::inactive};
    PoseSource poseSource{PoseSource::unavailable};
    std::uint64_t generation{};
    std::uint64_t activeSession{};
    bool installed{};
    bool requested{};
    bool active{};
    bool applied{};
    bool projectionAvailable{};
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

/**
 * Resolves and attaches the copied camera-pose and FOV boundaries.
 * @return True when both
 * exact-image replacements are attached.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Stops Viewer Camera and detaches both copied-output boundaries.
 * @return True when both
 * replacements drained and detached.
 */
[[nodiscard]] bool uninstall() noexcept;

/**
 * Polls input once after the stock camera frame.
 * @param playerIndex Current local-player
 * index, or the invalid player handle.
 */
void poll(std::uint32_t playerIndex) noexcept;

/**
 * Adds raw mouse motion captured by the existing Tiger Input Window subclass.
 * @param x
 * Horizontal raw-input delta.
 * @param y Vertical raw-input delta.
 */
void add_mouse_delta(long x, long y) noexcept;

/** @return True while Viewer Camera owns raw mouse motion. */
[[nodiscard]] bool captures_mouse() noexcept;

/**
 * Requests an enter or exit transition on the next camera poll.
 * @param active True to enter
 * Viewer Camera; false to restore the native camera.
 */
void request_active(bool active) noexcept;

/**
 * Moves the detached camera to a copied pose without changing the player.
 * @param pose Finite
 * detached-camera pose to apply.
 * @return True when Viewer Camera is active and the pose was
 * accepted.
 */
[[nodiscard]] bool move_to(const Pose& pose) noexcept;

/**
 * Moves the detached camera to the latest copied player position.
 * @return True when a
 * current local-player position was available and applied.
 */
[[nodiscard]] bool return_to_player() noexcept;

/** @return True from the enter request until exit completes. */
[[nodiscard]] bool requested() noexcept;

/** @return True only while a detached pose is applied. */
[[nodiscard]] bool active() noexcept;

/** @return Nonzero identity for the current active session, or zero while inactive. */
[[nodiscard]] std::uint64_t active_session() noexcept;

/**
 * Copies the detached pose without taking the Viewer state lock.
 * @param pose Destination for
 * the coherent copied pose.
 * @return True when the copied pose is finite and Viewer Camera is
 * active.
 */
[[nodiscard]] bool pose_snapshot(Pose& pose) noexcept;

/** @return One coherent copied runtime snapshot, using player pose while Viewer is inactive. */
[[nodiscard]] Status status() noexcept;

/**
 * Queues a copied path for camera-thread playback.
 * @param path Bounded path copied into the
 * camera-thread mailbox.
 * @return True when the path was valid and queued.
 */
[[nodiscard]] bool request_playback(const PlaybackPath& path) noexcept;
/**
 * Pauses or resumes the current path at its existing time.
 * @param paused True to pause;
 * false to resume.
 */
void request_playback_pause(bool paused) noexcept;
/** Stops path playback and clears its pending camera-thread command. */
void request_playback_stop() noexcept;
/**
 * Requests a bounded absolute playback time.
 * @param seconds Path time in seconds; the camera
 * thread clamps it to the duration.
 */
void request_playback_scrub(float seconds) noexcept;
/** @return One coherent copied path-playback status. */
[[nodiscard]] PlaybackStatus playback_status() noexcept;
/**
 * Consumes the oldest pending keyframe capture request.
 * @param request Destination for the
 * copied capture metadata.
 * @return True when a request was available.
 */
[[nodiscard]] bool consume_snapshot_capture_request(SnapshotCaptureRequest& request) noexcept;

/**
 * Returns the stable lifecycle label used by logs and UI.
 * @param phase Lifecycle phase to
 * describe.
 * @return Static null-terminated label.
 */
[[nodiscard]] const char* phase_name(Phase phase) noexcept;
/**
 * Returns the stable pose-source label used by logs and UI.
 * @param source Pose source to
 * describe.
 * @return Static null-terminated label.
 */
[[nodiscard]] const char* pose_source_name(PoseSource source) noexcept;
/**
 * Returns the stable failure label used by logs and UI.
 * @param failure Failure to describe.

 * * @return Static null-terminated label.
 */
[[nodiscard]] const char* failure_name(Failure failure) noexcept;

} // namespace sunrise::client::viewer::camera
