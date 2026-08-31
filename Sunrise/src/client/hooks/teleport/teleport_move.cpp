/**
 * The teleport itself. The camera hook publishes a forward vector and reads the bound key once a
 * frame. The physics hook applies the move before the sync it runs ahead of. Physics owns the
 * position, so writing the object placement would move the camera alone.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../player_hold/player_hold.h"
#include "../polled_input/runtime.h"
#include "internal.h"
#include "position_request_slot.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {

/**
 * Frames a press stays pending. Orbit and loading screens tick the camera but never the player's
 * physics, so a request with no limit is used up later and reads as a queued teleport.
 */
constexpr std::uint32_t kRequestLifetimeFrames = 3;
/** Frames an ordinary physics tick gets to collect a request before the forced path takes it. */
constexpr std::uint32_t kForceAfterFrames = 1;
constexpr std::uint64_t kPositionFailureLogIntervalMilliseconds = 5000;

/**
 * Frames the injected press is held. It has to survive at least one scan and one integration
 * step, or the move it exists to publish is never read.
 */
constexpr std::uint32_t kPressFrames = 2;
/** Authored action driven to wake the body. Forward is the gentlest one that moves it. */
constexpr std::uint16_t kForwardAction =
    static_cast<std::uint16_t>(state::account::settings::bindings::Action::moveForward);

std::atomic_bool g_requested{false};
std::atomic_bool g_forwardValid{false};
std::atomic_bool g_keyDown{false};
std::atomic_uint32_t g_requestAge{0};
/** Set while the feature is usable, so the per-tick path costs one atomic read when it is not. */
std::atomic_bool g_active{false};
std::atomic_bool g_destinationRequested{false};
SRWLOCK g_destinationLock = SRWLOCK_INIT;
position_request::Slot g_destinationRequests;
PositionRequestStatus g_positionStatus{};
std::atomic_uint64_t g_nextPositionSequence{1};
std::atomic_uint64_t g_lastPositionFailureLog{};

/**
 * The player's physics component, kept from the last tick that carried it. At rest the sync stops
 * being called for the player at all, so the pointer is the only way back to them.
 */
std::atomic<std::byte*> g_playerComponent{nullptr};
/** Frames left before the injected press is released. */
std::atomic_uint32_t g_pressFrames{0};

ControlledHandle g_controlledHandle{};
CameraSingleton g_cameraSingleton{};

/** Written by the camera hook and read by the physics hook. Both run on the same thread. */
std::array<float, kVectorLanes> g_forward{};

[[nodiscard]] std::uint64_t next_position_sequence() noexcept {
    std::uint64_t current = g_nextPositionSequence.load(std::memory_order_relaxed);
    while (current != (std::numeric_limits<std::uint64_t>::max)()) {
        if (g_nextPositionSequence.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

void finish_position_request(const position_request::Request& request,
                             PositionRequestPhase phase,
                             PositionRequestFailure failure) noexcept {
    AcquireSRWLockExclusive(&g_destinationLock);
    if (g_positionStatus.sequence == request.sequence) {
        g_positionStatus.phase = phase;
        g_positionStatus.failure = failure;
    }
    ReleaseSRWLockExclusive(&g_destinationLock);
}

void report_position_failure(std::uint64_t sequence, PositionRequestFailure failure) noexcept {
    const std::uint64_t now = GetTickCount64();
    std::uint64_t previous = g_lastPositionFailureLog.load(std::memory_order_relaxed);
    while (now - previous >= kPositionFailureLogIntervalMilliseconds) {
        if (g_lastPositionFailureLog.compare_exchange_weak(
                previous, now, std::memory_order_relaxed)) {
            std::array<char, 160> line{};
            const int written =
                std::snprintf(line.data(),
                              line.size(),
                              "ev=teleport stage=absolute result=reject sequence=%llu reason=%s",
                              static_cast<unsigned long long>(sequence),
                              position_request_failure_name(failure));
            if (written > 0) {
                core::log::write(
                    core::log::Channel::client,
                    core::log::Level::warn,
                    {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
            }
            return;
        }
    }
}

void reject_position_request(const position_request::Request& request,
                             PositionRequestFailure failure) noexcept {
    finish_position_request(request, PositionRequestPhase::rejected, failure);
    report_position_failure(request.sequence, failure);
}

[[nodiscard]] bool take_position_request(position_request::Request& request) noexcept {
    AcquireSRWLockExclusive(&g_destinationLock);
    const bool taken = g_destinationRequests.take(request);
    if (taken) {
        g_destinationRequested.store(false, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_destinationLock);
    return taken;
}

void reject_pending_position(PositionRequestFailure failure) noexcept {
    position_request::Request request{};
    AcquireSRWLockExclusive(&g_destinationLock);
    const bool taken = g_destinationRequests.take(request);
    if (taken && g_positionStatus.sequence == request.sequence) {
        g_positionStatus.phase = PositionRequestPhase::rejected;
        g_positionStatus.failure = failure;
    }
    g_destinationRequested.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_destinationLock);
    if (taken) {
        report_position_failure(request.sequence, failure);
    }
}

/**
 * Reads one value out of game memory without faulting on a torn pointer.
 * @param address Source address.
 * @param value Receives the value.
 * @return True when Windows copied the whole value.
 */
template <typename T> [[nodiscard]] bool read_at(const std::byte* address, T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/**
 * Writes one vector into game memory. The call applies page protection itself.
 * @param address Destination address.
 * @param value Three lanes to store.
 * @return True when Windows copied the whole vector.
 */
[[nodiscard]] bool write_vector(std::byte* address,
                                const std::array<float, kVectorLanes>& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T written = 0;
    const SIZE_T size = sizeof(float) * kVectorLanes;
    return WriteProcessMemory(GetCurrentProcess(), address, value.data(), size, &written) != FALSE
           && written == size;
}

/**
 * Finds the rigid body a physics component drives.
 * @param component Physics component.
 * @return The body, or null when the chain breaks.
 */
[[nodiscard]] std::byte* body_of(std::byte* component) noexcept {
    std::byte* array = nullptr;
    std::int32_t index = 0;
    if (!read_at(component + kPhysicsComponentBodyArray, array)
        || !read_at(component + kPhysicsComponentBodyIndex, index) || array == nullptr
        || index < 0) {
        return nullptr;
    }
    std::byte* body = nullptr;
    const std::size_t offset = kBodyEntryStride * static_cast<std::size_t>(index) + kBodyPointer;
    return read_at(array + offset, body) ? body : nullptr;
}

/**
 * Ages a pending request and drops it once nothing has taken it. A press is meant for the moment
 * it is made, so one that finds no player physics tick is dropped, not held for the next one.
 */
void expire_request() noexcept {
    if (!g_requested.load(std::memory_order_acquire)) {
        return;
    }
    if (g_requestAge.fetch_add(1, std::memory_order_relaxed) + 1 >= kRequestLifetimeFrames) {
        g_requested.store(false, std::memory_order_release);
        if (g_destinationRequested.load(std::memory_order_acquire)) {
            reject_pending_position(PositionRequestFailure::expired);
        }
    }
}

/**
 * Runs the whole move for a component already proved to be the player's.
 * @param component Physics component driving the player.
 * @return True when the body was found and its position was written.
 */
[[nodiscard]] bool perform_move(std::byte* component) noexcept;

/** @param reason Key naming the step that stopped the move. */
void report_skip(const char* reason) noexcept;

/**
 * Starts the injected press that wakes the body.
 * Nothing reads the new body position until something integrates it, so the move is published by
 * driving the player's own forward action rather than by writing what it would have produced.
 */
void begin_press() noexcept {
    const state::AccountState account = state::account_snapshot();
    const auto& binding = account.settings.keyBindings.values[kForwardAction];
    if (!binding.primary.has_value()) {
        return;
    }
    const std::uint32_t virtualKey = action_key(*binding.primary);
    if (virtualKey == 0) {
        report_skip("no_key");
        return;
    }
    hooks::polled_input::hold_key(virtualKey);
    g_pressFrames.store(kPressFrames, std::memory_order_release);
}

/** Releases the injected press once it has been scanned. */
void end_press() noexcept {
    if (g_pressFrames.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (g_pressFrames.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        hooks::polled_input::release_key();
    }
}

/** Cancels every deferred or injected move while Player Hold owns placement. */
void cancel_for_hold() noexcept {
    g_requested.store(false, std::memory_order_release);
    g_requestAge.store(0, std::memory_order_relaxed);
    g_keyDown.store(false, std::memory_order_relaxed);
    g_pressFrames.store(0, std::memory_order_release);
    hooks::polled_input::release_key();
}

/**
 * Reports the gate values the sync tests before it publishes a transform.
 *
 * The move lands while moving and does nothing at rest for all three write targets, so what
 * changes at rest is upstream of the write. These four gates are what the sync reads first.
 *
 * @param component Physics component owning the player.
 * @param body Rigid body behind it.
 */
void report_gates(const std::byte* component, const std::byte* body) noexcept {
    std::uint8_t suppressed = 0;
    std::int32_t bodyIndex = 0;
    std::uint32_t bodyFlags = 0;
    std::uint8_t motionType = 0;
    (void)read_at(component + kPhysicsComponentSuppress, suppressed);
    (void)read_at(component + kPhysicsComponentBodyIndex, bodyIndex);
    (void)read_at(body + kBodyFlags, bodyFlags);
    (void)read_at(body + kBodyMotionType, motionType);
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=gates suppress=%u index=%d "
                                      "flags=0x%08X active=%u motion=%u",
                                      static_cast<unsigned>(suppressed),
                                      static_cast<int>(bodyIndex),
                                      static_cast<unsigned>(bodyFlags),
                                      (bodyFlags & kBodyActiveBit) != 0 ? 1U : 0U,
                                      static_cast<unsigned>(motionType));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * @param component Candidate physics component.
 * @return True when it drives the object the local player controls.
 */
[[nodiscard]] bool owns_player(std::byte* component) noexcept {
    std::uint32_t controlled = kInvalidHandle;
    g_controlledHandle(&controlled);
    if (controlled == kInvalidHandle) {
        return false;
    }
    std::uint16_t owner = 0;
    return read_at(component + kPhysicsComponentObjectHandle, owner)
           && (controlled & kHandleIndexMask)
                  == (static_cast<std::uint32_t>(owner) & kHandleIndexMask);
}

/** @param reason Key naming the step that stopped the move. */
void report_skip(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=teleport stage=move result=skip reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Writes one vertical velocity, leaving run momentum on the other two lanes.
 * @param body Rigid body to write.
 * @param value Vertical velocity to store.
 */
void set_vertical_velocity(std::byte* body, float value) noexcept {
    std::array<float, kVectorLanes> velocity{};
    if (!read_at(body + kBodyVelocityX, velocity)) {
        return;
    }
    velocity[kVerticalLane] = value;
    (void)write_vector(body + kBodyVelocityX, velocity);
}

/**
 * Adds one world delta to a stored position.
 * @param address Vector to move.
 * @param delta World units per lane.
 * @param before Receives the value read.
 * @param after Receives the value written.
 * @return True when the new value was stored.
 */
[[nodiscard]] bool offset_vector(std::byte* address,
                                 const std::array<float, kVectorLanes>& delta,
                                 std::array<float, kVectorLanes>& before,
                                 std::array<float, kVectorLanes>& after) noexcept {
    if (!read_at(address, before)) {
        return false;
    }
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        after[lane] = before[lane] + delta[lane];
    }
    return write_vector(address, after);
}

/**
 * Adds the configured distance along the published forward vector.
 *
 * Only the rigid body is written. The physics component's own vector is composed against the body
 * orientation rather than added to it, so a world delta applied there corrupts the transform.
 *
 * @param body Rigid body being moved.
 * @param distance World units to travel.
 * @return True when the new position was stored.
 */
[[nodiscard]] bool move_body(std::byte* body, float distance) noexcept {
    std::array<float, kVectorLanes> delta{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        delta[lane] = g_forward[lane] * distance;
    }
    std::array<float, kVectorLanes> position{};
    std::array<float, kVectorLanes> moved{};
    if (!offset_vector(body + kBodyPositionX, delta, position, moved)) {
        report_skip("body");
        return false;
    }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=move result=ok dist=%.1f "
                                      "from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f",
                                      static_cast<double>(distance),
                                      static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2]),
                                      static_cast<double>(moved[0]),
                                      static_cast<double>(moved[1]),
                                      static_cast<double>(moved[2]));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/**
 * Runs the whole move for a component already proved to be the player's.
 * @param component Physics component driving the player.
 * @return True when the body was found and its position was written.
 */
[[nodiscard]] bool perform_move(std::byte* component) noexcept {
    std::byte* const body = body_of(component);
    if (body == nullptr) {
        report_skip("no_body");
        return false;
    }
    report_gates(component, body);
    set_vertical_velocity(body, 0.0F);
    if (!move_body(body, client::movement::get().distance)) {
        return false;
    }
    begin_press();
    return true;
}

[[nodiscard]] bool perform_destination(std::byte* component) noexcept {
    if (!g_destinationRequested.load(std::memory_order_acquire)) {
        return false;
    }
    position_request::Request request{};
    if (!take_position_request(request)) {
        return false;
    }
    state::activity::SessionSnapshot session{};
    if (!state::activity::snapshot_session(request.activitySession, session)
        || session.binding.createdRevision != request.activityRevision) {
        reject_position_request(request, PositionRequestFailure::sessionChanged);
        return false;
    }
    std::byte* const body = body_of(component);
    Vector before{};
    if (body == nullptr || !read_at(body + kBodyPositionX, before)) {
        reject_position_request(request, PositionRequestFailure::bodyUnavailable);
        return false;
    }
    report_gates(component, body);
    const hooks::player_hold::ReanchorResult reanchored =
        hooks::player_hold::reanchor(component, request.destination);
    if (reanchored == hooks::player_hold::ReanchorResult::failed) {
        reject_position_request(request, PositionRequestFailure::reanchorFailed);
        return false;
    }
    if (reanchored == hooks::player_hold::ReanchorResult::inactive) {
        if (!write_vector(body + kBodyVelocityX, Vector{})
            || !write_vector(body + kBodyPositionX, request.destination)) {
            reject_position_request(request, PositionRequestFailure::writeFailed);
            return false;
        }
        begin_press();
    }
    finish_position_request(request, PositionRequestPhase::succeeded, PositionRequestFailure::none);
    std::array<char, 192> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=absolute result=ok sequence=%llu "
                                      "from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f",
                                      static_cast<unsigned long long>(request.sequence),
                                      static_cast<double>(before[0]),
                                      static_cast<double>(before[1]),
                                      static_cast<double>(before[2]),
                                      static_cast<double>(request.destination[0]),
                                      static_cast<double>(request.destination[1]),
                                      static_cast<double>(request.destination[2]));
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
    return true;
}

} // namespace

/** Publishes the two functions the hooks call. */
void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept {
    g_controlledHandle = controlled;
    g_cameraSingleton = singleton;
}

/** Drops those functions and every latched request. */
void clear_targets() noexcept {
    if (g_destinationRequested.load(std::memory_order_acquire)) {
        reject_pending_position(PositionRequestFailure::shutdown);
    }
    g_controlledHandle = nullptr;
    g_cameraSingleton = nullptr;
    g_requested.store(false, std::memory_order_release);
    g_forwardValid.store(false, std::memory_order_release);
    g_keyDown.store(false, std::memory_order_relaxed);
    g_requestAge.store(0, std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
    g_destinationRequested.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_destinationLock);
    g_destinationRequests.clear();
    ReleaseSRWLockExclusive(&g_destinationLock);
    g_playerComponent.store(nullptr, std::memory_order_relaxed);
}

/** Copies the native source selected for one player's current camera frame. */
bool camera_identity(std::uint32_t playerIndex, CameraIdentity& identity) noexcept {
    identity = {};
    if (playerIndex == kInvalidHandle || g_cameraSingleton == nullptr) {
        return false;
    }
    std::byte* const camera = g_cameraSingleton();
    if (camera == nullptr) {
        return false;
    }
    const std::byte* const block = camera + kCameraBlockStride * playerIndex;
    return read_at(block + kCameraSourceHandle, identity.sourceHandle)
           && read_at(block + kCameraSourceClass, identity.sourceClass);
}

/** Publishes the camera forward vector for the physics tick that follows. */
void capture_forward(std::uint32_t playerIndex) noexcept {
    if (playerIndex == kInvalidHandle || g_cameraSingleton == nullptr) {
        return;
    }
    std::byte* const camera = g_cameraSingleton();
    if (camera == nullptr) {
        return;
    }
    std::array<float, kVectorLanes> forward{};
    if (!read_at(camera + kCameraBlockStride * playerIndex + kCameraForwardX, forward)) {
        return;
    }
    g_forward = forward;
    g_forwardValid.store(true, std::memory_order_release);
}

/** Latches one teleport request if the bound key went down this frame. */
void poll_request() noexcept {
    end_press();
    const bool absolute = g_destinationRequested.load(std::memory_order_acquire);
    if (hooks::player_hold::blocks_teleport() && !absolute) {
        cancel_for_hold();
        return;
    }
    expire_request();
    if (absolute) {
        g_keyDown.store(false, std::memory_order_relaxed);
        return;
    }
    const client::movement::Settings settings = client::movement::get();
    const bool usable = settings.enabled && settings.virtualKey != client::movement::kNoKey;
    g_active.store(usable, std::memory_order_relaxed);
    if (!usable) {
        g_keyDown.store(false, std::memory_order_relaxed);
        return;
    }
    // An open interface owns the keyboard, so the key that binds the teleport must not fire it.
    if (core::ui::runtime::snapshot().visible) {
        g_keyDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down = client::input::game_focused()
                      && (GetAsyncKeyState(static_cast<int>(settings.virtualKey)) & 0x8000) != 0;
    if (down && !g_keyDown.exchange(down, std::memory_order_relaxed)) {
        g_destinationRequested.store(false, std::memory_order_release);
        g_requestAge.store(0, std::memory_order_relaxed);
        g_requested.store(true, std::memory_order_release);
        return;
    }
    g_keyDown.store(down, std::memory_order_relaxed);
}

/** Moves the local player if a request is pending and this component owns them. */
void apply_pending(void* component) noexcept {
    const bool absolute = g_destinationRequested.load(std::memory_order_acquire);
    if (hooks::player_hold::blocks_teleport() && !absolute) {
        cancel_for_hold();
        return;
    }
    if ((!g_active.load(std::memory_order_relaxed) && !absolute) || component == nullptr
        || g_controlledHandle == nullptr) {
        return;
    }
    const bool requested = g_requested.load(std::memory_order_acquire);
    // The ownership test runs per component, so it is paid only while a request is open or until
    // the player's component is known. Once it is known, an ordinary tick costs two atomic reads.
    if (!requested && g_playerComponent.load(std::memory_order_relaxed) != nullptr) {
        return;
    }
    if (!owns_player(static_cast<std::byte*>(component))) {
        return;
    }
    std::byte* const physics = static_cast<std::byte*>(component);
    g_playerComponent.store(physics, std::memory_order_relaxed);
    if (!requested || (!absolute && !g_forwardValid.load(std::memory_order_acquire))) {
        return;
    }
    g_requested.store(false, std::memory_order_release);
    (void)(absolute ? perform_destination(physics) : perform_move(physics));
}

/** Runs the move for a request no physics tick collected. */
void force_pending() noexcept {
    const bool absolute = g_destinationRequested.load(std::memory_order_acquire);
    if (hooks::player_hold::blocks_teleport() && !absolute) {
        cancel_for_hold();
        return;
    }
    if (!g_requested.load(std::memory_order_acquire)
        || (!absolute && !g_forwardValid.load(std::memory_order_acquire))
        || g_requestAge.load(std::memory_order_relaxed) < kForceAfterFrames) {
        return;
    }
    std::byte* const physics = g_playerComponent.load(std::memory_order_relaxed);
    // The cached pointer outlives a destination change, so it is proved again before use.
    if (physics == nullptr || g_controlledHandle == nullptr || !owns_player(physics)) {
        return;
    }
    if (!absolute) {
        g_requested.store(false, std::memory_order_release);
        if (!perform_move(physics)) {
            return;
        }
        invoke_sync(physics);
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=teleport stage=force result=ok");
        return;
    }
    const PositionRequestStatus before = position_request_status();
    invoke_sync(physics);
    const PositionRequestStatus after = position_request_status();
    if (before.sequence == 0 || after.sequence != before.sequence
        || after.phase != PositionRequestPhase::succeeded) {
        return;
    }
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=teleport stage=force result=ok");
}

bool request_position(const Vector& position,
                      std::uint64_t activitySession,
                      std::uint64_t& sequence) noexcept {
    sequence = 0;
    for (const float lane : position) {
        if (!std::isfinite(lane)) {
            return false;
        }
    }
    state::activity::SessionSnapshot session{};
    if (activitySession == 0 || !state::activity::snapshot_session(activitySession, session)
        || session.binding.createdRevision == 0) {
        return false;
    }
    sequence = next_position_sequence();
    if (sequence == 0) {
        return false;
    }
    const position_request::Request request{
        position, sequence, activitySession, session.binding.createdRevision};
    AcquireSRWLockExclusive(&g_destinationLock);
    const bool published = g_destinationRequests.publish(request);
    if (published) {
        g_positionStatus = PositionRequestStatus{position,
                                                 sequence,
                                                 activitySession,
                                                 PositionRequestPhase::pending,
                                                 PositionRequestFailure::none};
        g_requestAge.store(0, std::memory_order_relaxed);
        g_destinationRequested.store(true, std::memory_order_release);
        g_requested.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_destinationLock);
    if (!published) {
        sequence = 0;
        return false;
    }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=teleport stage=absolute result=queued sequence=%llu "
                                      "to=%.1f,%.1f,%.1f",
                                      static_cast<unsigned long long>(sequence),
                                      position[0],
                                      position[1],
                                      position[2]);
    if (written > 0) {
        core::log::write(
            core::log::Channel::client,
            core::log::Level::info,
            {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1U)});
    }
    // A stationary player has no physics sync to collect the request. Wake the body through the
    // authored movement scan so the next sync can resolve its component and apply the destination.
    // The normal expiry remains in force if the input hook or sync is unavailable.
    if (local_player_available()) {
        begin_press();
    }
    return true;
}

PositionRequestStatus position_request_status() noexcept {
    AcquireSRWLockShared(&g_destinationLock);
    const PositionRequestStatus result = g_positionStatus;
    ReleaseSRWLockShared(&g_destinationLock);
    return result;
}

const char* position_request_phase_name(PositionRequestPhase phase) noexcept {
    switch (phase) {
    case PositionRequestPhase::idle:
        return "idle";
    case PositionRequestPhase::pending:
        return "pending";
    case PositionRequestPhase::succeeded:
        return "succeeded";
    case PositionRequestPhase::rejected:
        return "rejected";
    }
    return "unknown";
}

const char* position_request_failure_name(PositionRequestFailure failure) noexcept {
    switch (failure) {
    case PositionRequestFailure::none:
        return "none";
    case PositionRequestFailure::nonFinite:
        return "non-finite destination";
    case PositionRequestFailure::sessionUnavailable:
        return "activity session unavailable";
    case PositionRequestFailure::sessionChanged:
        return "activity session changed";
    case PositionRequestFailure::playerUnavailable:
        return "controlled player unavailable";
    case PositionRequestFailure::bodyUnavailable:
        return "player body unavailable";
    case PositionRequestFailure::writeFailed:
        return "player placement write failed";
    case PositionRequestFailure::reanchorFailed:
        return "Viewer hold reanchor failed";
    case PositionRequestFailure::expired:
        return "request expired before a player physics sync";
    case PositionRequestFailure::shutdown:
        return "teleport runtime shut down";
    }
    return "unknown";
}

/** Reports the physics component the local player was last seen driving. */
void* local_player_component() noexcept {
    return g_playerComponent.load(std::memory_order_relaxed);
}

/** @param component Candidate physics component. @return True when the local player drives it. */
bool owns_local_player(void* component) noexcept {
    return component != nullptr && g_controlledHandle != nullptr
           && owns_player(static_cast<std::byte*>(component));
}

/** Resolves a transient rigid body for an already qualified component. */
void* transient_body(void* component) noexcept {
    return component != nullptr ? body_of(static_cast<std::byte*>(component)) : nullptr;
}

/** @return True while the game publishes a controlled local player. */
bool local_player_available() noexcept {
    std::uint32_t controlled = kInvalidHandle;
    return controlled_player_handle(controlled);
}

/** Copies the current controlled-object handle. */
bool controlled_player_handle(std::uint32_t& handle) noexcept {
    handle = kInvalidHandle;
    if (g_controlledHandle == nullptr) {
        return false;
    }
    g_controlledHandle(&handle);
    return handle != kInvalidHandle;
}

/** Reads the world position of the body a physics component drives. */
bool read_position(void* component, Vector& position) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::byte* const body = body_of(static_cast<std::byte*>(component));
    return body != nullptr && read_at(body + kBodyPositionX, position);
}

/** Writes the world position of the body a physics component drives. */
bool write_position(void* component, const Vector& position) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::byte* const body = body_of(static_cast<std::byte*>(component));
    return body != nullptr && write_vector(body + kBodyPositionX, position);
}

/** Reads the linear velocity of the body a physics component drives. */
bool read_velocity(void* component, Vector& velocity) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::byte* const body = body_of(static_cast<std::byte*>(component));
    return body != nullptr && read_at(body + kBodyVelocityX, velocity);
}

/** Writes the linear velocity of the body a physics component drives. */
bool write_velocity(void* component, const Vector& velocity) noexcept {
    if (component == nullptr) {
        return false;
    }
    std::byte* const body = body_of(static_cast<std::byte*>(component));
    return body != nullptr && write_vector(body + kBodyVelocityX, velocity);
}

/** Reports the camera forward vector published this frame. */
bool camera_forward(Vector& forward) noexcept {
    if (!g_forwardValid.load(std::memory_order_acquire)) {
        return false;
    }
    forward = g_forward;
    return true;
}

} // namespace sunrise::client::hooks::teleport
