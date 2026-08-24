#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::teleport {

/** Three floats make one position or velocity vector. */
inline constexpr std::size_t kVectorLanes = 3;
/** The vertical lane. The camera basis is X forward, Z up, so the third lane is up. */
inline constexpr std::size_t kVerticalLane = 2;

/** One world-space vector as the game stores it. */
using Vector = std::array<float, kVectorLanes>;

/** Writes the local player's controlled-object handle, or the invalid sentinel. */
using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);
/** Returns the camera pose block array. The pointer in its global is obfuscated, so we call it. */
using CameraSingleton = std::byte* (*)();

/** Copied identity of the native source selected for one player's camera frame. */
struct CameraIdentity {
    std::uint32_t sourceHandle{};
    std::uint32_t sourceClass{};
};

enum class PositionRequestPhase : std::uint8_t {
    idle,
    pending,
    succeeded,
    rejected,
};

enum class PositionRequestFailure : std::uint8_t {
    none,
    nonFinite,
    sessionUnavailable,
    sessionChanged,
    playerUnavailable,
    bodyUnavailable,
    writeFailed,
    reanchorFailed,
    expired,
    shutdown,
};

struct PositionRequestStatus final {
    Vector destination{};
    std::uint64_t sequence{};
    std::uint64_t activitySession{};
    PositionRequestPhase phase{PositionRequestPhase::idle};
    PositionRequestFailure failure{PositionRequestFailure::none};
};

/**
 * Publishes the two functions the hooks call.
 * @param controlled Writes the local player's object handle.
 * @param singleton Returns the camera pose block array.
 */
void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept;

/** Drops those functions and every latched request. */
void clear_targets() noexcept;

/**
 * Attaches the camera and physics hooks that carry the teleport.
 * @return True when all three targets were found and both detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches both teleport hooks. */
void uninstall() noexcept;

/**
 * Publishes the camera forward vector for the physics tick that follows.
 * @param playerIndex Player the camera pose block belongs to.
 */
void capture_forward(std::uint32_t playerIndex) noexcept;

/** Latches one teleport request if the bound key went down this frame. */
void poll_request() noexcept;

/** Runs the move for a request no physics tick collected, and drives the sync for it. */
void force_pending() noexcept;

/**
 * Queues an unrestricted absolute local-player move for the next validated physics sync.
 *
 * Only finite coordinates and an exact live activity-session generation are accepted.
 * @param
 * position Finite world-space destination.
 * @param activitySession Expected current
 * activity-session identity.
 * @param sequence Receives the monotonic request identity on
 * success.
 * @return True when the request was validated and queued.
 */
[[nodiscard]] bool request_position(const Vector& position,
                                    std::uint64_t activitySession,
                                    std::uint64_t& sequence) noexcept;

/** @return Latest absolute-position request state without retained gameplay pointers. */
[[nodiscard]] PositionRequestStatus position_request_status() noexcept;
/**
 * @param phase Request phase to describe.
 * @return Static null-terminated phase label.
 */
[[nodiscard]] const char* position_request_phase_name(PositionRequestPhase phase) noexcept;
/**
 * @param failure Request failure to describe.
 * @return Static null-terminated failure label.

 */
[[nodiscard]] const char* position_request_failure_name(PositionRequestFailure failure) noexcept;

/**
 * Finds both key tables from the polled keyboard scan.
 * @return True when the scan and both of its table loads were found.
 */
[[nodiscard]] bool resolve_action_keys() noexcept;

/** Drops both key tables. */
void clear_action_keys() noexcept;

/** @return True after both authored-action key tables have been resolved. */
[[nodiscard]] bool action_keys_ready() noexcept;

/**
 * Turns one authored binding into the virtual key the scan will read.
 * @param binding Input code taken from an authored binding half.
 * @return The virtual key, or 0 when there is none.
 */
[[nodiscard]] std::uint32_t action_key(std::uint16_t binding) noexcept;

/**
 * Calls the physics sync for one component through the installed trampoline.
 * @param component Physics component to sync.
 */
void invoke_sync(void* component) noexcept;

/**
 * Moves the local player if a request is pending and this component owns them.
 * @param component Physics component about to be synced.
 */
void apply_pending(void* component) noexcept;

/**
 * @param component Candidate physics component.
 * @return True when it drives the object the local player controls.
 *
 * Exposed because the physics sync is the only tick that sees every component, and a feature that
 * has to act on the player's own tick needs the same test this module already performs.
 */
[[nodiscard]] bool owns_local_player(void* component) noexcept;

/**
 * Resolves the current rigid body for an already qualified component.
 * The result is transient
 * game memory and must not outlive the caller's engine callback.
 * @param component Qualified live
 * physics component.
 * @return Transient rigid body, or null when current ownership cannot be
 * proven.
 */
[[nodiscard]] void* transient_body(void* component) noexcept;

/** @return True while the game publishes a controlled local player. */
[[nodiscard]] bool local_player_available() noexcept;

/**
 * Copies the current controlled-object handle without retaining a gameplay pointer.
 * @param
 * handle Receives the current object handle.
 * @return True when the controlled local-player
 * handle is valid.
 */
[[nodiscard]] bool controlled_player_handle(std::uint32_t& handle) noexcept;

/**
 * Reads the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_position(void* component, Vector& position) noexcept;

/**
 * Reports the physics component the local player was last seen driving.
 * The sync stops for a player at rest, so a frame poll has no other way back to them.
 * @return That component, or null before the player has been seen. Prove it before use.
 */
[[nodiscard]] void* local_player_component() noexcept;

/**
 * Writes the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_position(void* component, const Vector& position) noexcept;

/**
 * Reads the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_velocity(void* component, Vector& velocity) noexcept;

/**
 * Writes the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_velocity(void* component, const Vector& velocity) noexcept;

/**
 * Copies the native source selected for one player's current camera frame.
 * @param playerIndex
 * Camera block index observed by the owning callback.
 * @param identity Receives scalar source
 * identity only.
 * @return True when the camera block and source identity are current and valid.

 */
[[nodiscard]] bool camera_identity(std::uint32_t playerIndex, CameraIdentity& identity) noexcept;

/**
 * The camera hook is the only site that sees the pose block, so it publishes the vector here.
 * @param forward Receives the camera forward vector published this frame.
 * @return True once the camera hook has published one.
 */
[[nodiscard]] bool camera_forward(Vector& forward) noexcept;

} // namespace sunrise::client::hooks::teleport
