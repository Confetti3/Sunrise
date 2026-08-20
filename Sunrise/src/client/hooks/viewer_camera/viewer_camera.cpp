#include "viewer_camera.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../hooking/detour.h"
#include "../../input/window_focus.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_position.h"
#include "../../player/player_settings_store.h"
#include "../../viewer/viewer_camera_settings_store.h"
#include "../../viewer/viewer_input_ownership.h"
#include "../polled_input/runtime.h"
#include "../teleport/runtime.h"

namespace sunrise::client::viewer::camera {
namespace {

namespace bindings = state::account::settings::bindings;
namespace window_input = client::input;
namespace workspace_input = client::viewer::input;

constexpr std::string_view kFovCopyText =
    "40 53 48 83 EC 20 E8 ? ? ? ? 48 8B D8 48 85 C0 74 1F F3 0F 10 80 D4 05 00 00 "
    "E8 ? ? ? ? 84 C0";
constexpr auto kFovCopy =
    patterns::signature<patterns::signature_length(kFovCopyText)>(kFovCopyText);
constexpr std::string_view kPoseCopyText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 48 8B F2 48 8B D9 "
    "E8 ? ? ? ? 48 8B C8 48 85 C0";
constexpr auto kPoseCopy =
    patterns::signature<patterns::signature_length(kPoseCopyText)>(kPoseCopyText);

constexpr std::size_t kHandleCount = 2;
constexpr std::size_t kFovSlot = 0;
constexpr std::size_t kPoseSlot = 1;
constexpr std::uint32_t kInvalidPlayer = std::numeric_limits<std::uint32_t>::max();
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
constexpr std::uint64_t kMaximumFrameMilliseconds = 50;
constexpr float kMinimumVectorLengthSquared = 0.000001F;
constexpr float kPitchLimit = 1.55334303427495F;

[[nodiscard]] constexpr float smoothstep(float value) noexcept {
    return value * value * (3.0F - 2.0F * value);
}
static_assert(smoothstep(0.0F) == 0.0F);
static_assert(smoothstep(1.0F) == 1.0F);

using FovCopy = float(__fastcall*)();
using PoseCopy = bool(__fastcall*)(float*, float*, float*);

struct PresentationSession {
    client::player::Settings before{};
    client::player::Settings applied{};
    bool hideWeaponOwned{};
    bool removeHudOwned{};
    bool active{};
};

struct RuntimeState {
    Pose pose{};
    PresentationSession presentation{};
    std::uint32_t playerIndex{kInvalidPlayer};
    std::uint32_t controlledHandle{kInvalidPlayer};
    hooks::teleport::CameraIdentity cameraIdentity{};
    std::uint64_t generation{};
    std::uint64_t activeSession{};
    std::uint64_t lastTick{};
    PlaybackPath playback{};
    float playbackElapsed{};
    float playbackScrub{-1.0F};
    std::size_t playbackKeyframe{};
    bool playbackPlaying{};
    bool playbackPaused{};
    bool playbackStartRequested{};
    bool playbackStopRequested{};
    bool active{};
    bool applied{};
    bool toggleDown{};
    bool transitioned{};
};

/** Camera-relative actions whose game reads are claimed while Viewer is active. */
constexpr std::array<bindings::Action, 7> kMovementActions{
    bindings::Action::moveForward,
    bindings::Action::moveBackward,
    bindings::Action::moveLeft,
    bindings::Action::moveRight,
    bindings::Action::jump,
    bindings::Action::toggleCrouch,
    bindings::Action::holdCrouch,
};

std::array<hooking::detour::Handle, kHandleCount> g_handles{};
std::atomic<FovCopy> g_originalFov{};
std::atomic<PoseCopy> g_originalPose{};
std::atomic_bool g_installPublishing{};
std::atomic_bool g_installed{};
std::atomic_bool g_stopping{};
std::atomic_bool g_requested{};
std::atomic_bool g_active{};
std::atomic_uint64_t g_activeSession{};
std::atomic_uint64_t g_nextSession{1};
std::atomic_uint32_t g_poseSequence{};
Pose g_publishedPose{};
std::atomic_bool g_posePresent{};
std::atomic_uint g_replacementInFlight{};
std::atomic_uint32_t g_framePlayer{kInvalidPlayer};
std::atomic_long g_mouseX{};
std::atomic_long g_mouseY{};
std::atomic<float> g_configuredFov{kNativeFov};
std::atomic<float> g_playbackFov{kNativeFov};
std::atomic_bool g_playbackFovActive{};
SRWLOCK g_captureLock{SRWLOCK_INIT};
std::array<SnapshotCaptureRequest, kMaximumPlaybackKeyframes> g_captureRequests{};
std::size_t g_captureHead{};
std::size_t g_captureCount{};
std::uint64_t g_captureSequence{};
std::atomic<float> g_outputFov{};
std::atomic<Failure> g_failure{Failure::none};
SRWLOCK g_stateLock{SRWLOCK_INIT};
RuntimeState g_state{};
std::array<bindings::Binding, kMovementActions.size()> g_bindings{};
bool g_bindingsRead{};

struct ReplacementScope {
    ReplacementScope() noexcept {
        g_replacementInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ReplacementScope() {
        g_replacementInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
    ReplacementScope(const ReplacementScope&) = delete;
    ReplacementScope& operator=(const ReplacementScope&) = delete;
};

[[nodiscard]] bool replacements_idle() noexcept {
    return g_replacementInFlight.load(std::memory_order_acquire) == 0;
}

void report(const char* stage, const char* result, const char* reason = nullptr) noexcept {
    std::array<char, 128> line{};
    const int written =
        reason == nullptr
            ? std::snprintf(
                  line.data(), line.size(), "ev=viewer_camera stage=%s result=%s", stage, result)
            : std::snprintf(line.data(),
                            line.size(),
                            "ev=viewer_camera stage=%s result=%s reason=%s",
                            stage,
                            result,
                            reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool finite(const Vector& value) noexcept {
    return std::ranges::all_of(value, [](float lane) { return std::isfinite(lane); });
}

[[nodiscard]] float length_squared(const Vector& value) noexcept {
    float result = 0.0F;
    for (const float lane : value) {
        result += lane * lane;
    }
    return result;
}

[[nodiscard]] bool normalize(Vector& value) noexcept {
    const float squared = length_squared(value);
    if (!std::isfinite(squared) || squared <= kMinimumVectorLengthSquared) {
        return false;
    }
    const float inverse = 1.0F / std::sqrt(squared);
    for (float& lane : value) {
        lane *= inverse;
    }
    return true;
}

[[nodiscard]] Vector right_of(const Vector& forward) noexcept {
    Vector right{forward[1], -forward[0], 0.0F};
    if (!normalize(right)) {
        return Vector{};
    }
    return right;
}

void rebuild_basis(Pose& pose) noexcept {
    const float horizontal = std::cos(pose.pitch);
    pose.forward = {
        horizontal * std::cos(pose.yaw), horizontal * std::sin(pose.yaw), std::sin(pose.pitch)};
    const Vector right = right_of(pose.forward);
    pose.up = {
        right[1] * pose.forward[2],
        -right[0] * pose.forward[2],
        right[0] * pose.forward[1] - right[1] * pose.forward[0],
    };
    (void)normalize(pose.up);
}

[[nodiscard]] float playback_duration(const PlaybackPath& path) noexcept {
    if (path.keyframeCount == 0) {
        return 0.0F;
    }
    float duration = path.keyframes[path.keyframeCount - 1].dwellSeconds;
    for (std::size_t index = 0; index + 1 < path.keyframeCount; ++index) {
        duration += path.keyframes[index].dwellSeconds + path.keyframes[index].travelSeconds;
    }
    return duration;
}

[[nodiscard]] bool valid_playback(const PlaybackPath& path) noexcept {
    if (path.keyframeCount == 0 || path.keyframeCount > path.keyframes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < path.keyframeCount; ++index) {
        const PlaybackKeyframe& keyframe = path.keyframes[index];
        if (!finite(keyframe.pose.position) || !std::isfinite(keyframe.pose.yaw)
            || !std::isfinite(keyframe.pose.pitch) || !std::isfinite(keyframe.pose.fov)
            || keyframe.pose.pitch < -kPitchLimit || keyframe.pose.pitch > kPitchLimit
            || keyframe.pose.fov < kMinimumFov || keyframe.pose.fov > kMaximumFov
            || !std::isfinite(keyframe.travelSeconds) || keyframe.travelSeconds < 0.0F
            || !std::isfinite(keyframe.dwellSeconds) || keyframe.dwellSeconds < 0.0F) {
            return false;
        }
    }
    const float duration = playback_duration(path);
    return std::isfinite(duration) && (!path.loop || duration > 0.0F);
}

[[nodiscard]] Pose playback_pose(const PlaybackPath& path,
                                 float elapsed,
                                 std::size_t& keyframeIndex) noexcept {
    Pose result = path.keyframes[0].pose;
    keyframeIndex = 0;
    float remaining = (std::max)(0.0F, elapsed);
    for (std::size_t index = 0; index < path.keyframeCount; ++index) {
        const PlaybackKeyframe& from = path.keyframes[index];
        if (remaining <= from.dwellSeconds || index + 1 == path.keyframeCount) {
            keyframeIndex = index;
            return from.pose;
        }
        remaining -= from.dwellSeconds;
        const float travel = from.travelSeconds;
        if (remaining <= travel) {
            const PlaybackKeyframe& to = path.keyframes[index + 1];
            const float linear = travel <= 0.0F ? 1.0F : std::clamp(remaining / travel, 0.0F, 1.0F);
            const float blend = smoothstep(linear);
            for (std::size_t lane = 0; lane < result.position.size(); ++lane) {
                result.position[lane] = from.pose.position[lane]
                                        + (to.pose.position[lane] - from.pose.position[lane]) * blend;
            }
            const float yawDelta = std::remainder(to.pose.yaw - from.pose.yaw,
                                                  2.0F * std::numbers::pi_v<float>);
            result.yaw = from.pose.yaw + yawDelta * blend;
            result.pitch = std::clamp(from.pose.pitch + (to.pose.pitch - from.pose.pitch) * blend,
                                      -kPitchLimit, kPitchLimit);
            result.fov = from.pose.fov + (to.pose.fov - from.pose.fov) * blend;
            rebuild_basis(result);
            keyframeIndex = index;
            return result;
        }
        remaining -= travel;
    }
    keyframeIndex = path.keyframeCount - 1;
    return path.keyframes[keyframeIndex].pose;
}

void clear_playback_locked() noexcept {
    g_state.playback = {};
    g_state.playbackElapsed = 0.0F;
    g_state.playbackScrub = -1.0F;
    g_state.playbackKeyframe = 0;
    g_state.playbackPlaying = false;
    g_state.playbackPaused = false;
    g_state.playbackStartRequested = false;
    g_state.playbackStopRequested = false;
    g_playbackFovActive.store(false, std::memory_order_release);
}

void enqueue_capture(std::size_t keyframeIndex) noexcept {
    AcquireSRWLockExclusive(&g_captureLock);
    if (g_captureCount == g_captureRequests.size()) {
        g_captureHead = (g_captureHead + 1U) % g_captureRequests.size();
        --g_captureCount;
    }
    const std::size_t tail = (g_captureHead + g_captureCount) % g_captureRequests.size();
    SnapshotCaptureRequest& request = g_captureRequests[tail];
    request = {};
    request.sequence = ++g_captureSequence;
    request.cameraSession = g_state.activeSession;
    request.keyframeIndex = keyframeIndex;
    request.pose = g_state.playback.keyframes[keyframeIndex].pose;
    request.pathName = g_state.playback.name;
    request.keyframeLabel = g_state.playback.keyframes[keyframeIndex].label;
    ++g_captureCount;
    ReleaseSRWLockExclusive(&g_captureLock);
}

void enqueue_capture_range(const PlaybackPath& path,
                           float after,
                           float through,
                           bool includeFirst) noexcept {
    float arrival = 0.0F;
    for (std::size_t index = 0; index < path.keyframeCount; ++index) {
        const bool crossed = (includeFirst && arrival == 0.0F)
                             || (arrival > after && arrival <= through);
        if (crossed && path.keyframes[index].captureSnapshot) {
            enqueue_capture(index);
        }
        if (index + 1 < path.keyframeCount) {
            arrival += path.keyframes[index].dwellSeconds + path.keyframes[index].travelSeconds;
        }
    }
}

[[nodiscard]] bool
valid_pose(const Vector& position, const Vector& forward, const Vector& up) noexcept {
    Vector normalizedForward = forward;
    Vector normalizedUp = up;
    return finite(position) && finite(forward) && finite(up) && normalize(normalizedForward)
           && normalize(normalizedUp);
}

/** Publishes the lock-owned pose through the same seqlock pattern as player position. */
void publish_pose_locked() noexcept {
    Pose pose = g_state.pose;
    pose.fov = g_playbackFovActive.load(std::memory_order_acquire)
                   ? g_playbackFov.load(std::memory_order_acquire)
                   : g_outputFov.load(std::memory_order_acquire);
    g_poseSequence.fetch_add(1, std::memory_order_acq_rel);
    g_publishedPose = pose;
    g_poseSequence.fetch_add(1, std::memory_order_release);
    g_posePresent.store(g_state.active, std::memory_order_release);
}

[[nodiscard]] FovCopy original_fov() noexcept {
    FovCopy next = g_originalFov.load(std::memory_order_acquire);
    while (next == nullptr && g_installPublishing.load(std::memory_order_acquire)) {
        SwitchToThread();
        next = g_originalFov.load(std::memory_order_acquire);
    }
    return next;
}

[[nodiscard]] PoseCopy original_pose() noexcept {
    PoseCopy next = g_originalPose.load(std::memory_order_acquire);
    while (next == nullptr && g_installPublishing.load(std::memory_order_acquire)) {
        SwitchToThread();
        next = g_originalPose.load(std::memory_order_acquire);
    }
    return next;
}

float __fastcall copy_fov() noexcept {
    ReplacementScope scope{};
    const FovCopy next = original_fov();
    const float native = next != nullptr ? next() : kNativeFov;
    if (!std::isfinite(native)) {
        return native;
    }
    float output = native;
    if (!g_stopping.load(std::memory_order_acquire) && g_active.load(std::memory_order_acquire)) {
        const float configured = g_playbackFovActive.load(std::memory_order_acquire)
                                     ? g_playbackFov.load(std::memory_order_acquire)
                                     : g_configuredFov.load(std::memory_order_acquire);
        if (configured >= kMinimumFov && configured <= kMaximumFov) {
            output = configured;
        }
    }
    g_outputFov.store(output, std::memory_order_release);
    return output;
}

bool __fastcall copy_pose(float* position, float* forward, float* up) noexcept {
    ReplacementScope scope{};
    const PoseCopy next = original_pose();
    if (next == nullptr) {
        return false;
    }
    const bool nativeValid = next(position, forward, up);
    if (!nativeValid || position == nullptr || forward == nullptr || up == nullptr) {
        g_failure.store(Failure::nativePose, std::memory_order_release);
        return nativeValid;
    }

    Vector nativePosition{position[0], position[1], position[2]};
    Vector nativeForward{forward[0], forward[1], forward[2]};
    Vector nativeUp{up[0], up[1], up[2]};
    if (!valid_pose(nativePosition, nativeForward, nativeUp)) {
        g_failure.store(Failure::nativePose, std::memory_order_release);
        return nativeValid;
    }
    if (g_stopping.load(std::memory_order_acquire)
        || !g_requested.load(std::memory_order_acquire)) {
        return nativeValid;
    }

    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_state.active) {
        const std::uint32_t playerIndex = g_framePlayer.load(std::memory_order_acquire);
        std::uint32_t controlledHandle = kInvalidPlayer;
        if (playerIndex == kInvalidPlayer
            || !hooks::teleport::controlled_player_handle(controlledHandle)) {
            ReleaseSRWLockExclusive(&g_stateLock);
            g_failure.store(Failure::localPlayer, std::memory_order_release);
            return nativeValid;
        }
        hooks::teleport::CameraIdentity cameraIdentity{};
        if (!hooks::teleport::camera_identity(playerIndex, cameraIdentity)) {
            ReleaseSRWLockExclusive(&g_stateLock);
            g_failure.store(Failure::cameraTarget, std::memory_order_release);
            return nativeValid;
        }
        g_state.pose.position = nativePosition;
        g_state.pose.forward = nativeForward;
        g_state.pose.up = nativeUp;
        (void)normalize(g_state.pose.forward);
        (void)normalize(g_state.pose.up);
        g_state.pose.yaw = std::atan2(g_state.pose.forward[1], g_state.pose.forward[0]);
        g_state.pose.pitch = std::asin(std::clamp(g_state.pose.forward[2], -1.0F, 1.0F));
        g_state.pose.fov = g_outputFov.load(std::memory_order_acquire);
        g_state.playerIndex = playerIndex;
        g_state.controlledHandle = controlledHandle;
        g_state.cameraIdentity = cameraIdentity;
        g_state.lastTick = GetTickCount64();
        g_state.active = true;
        ++g_state.generation;
        std::uint64_t session = g_nextSession.fetch_add(1, std::memory_order_acq_rel);
        if (session == 0) {
            session = g_nextSession.fetch_add(1, std::memory_order_acq_rel);
        }
        g_state.activeSession = session;
        g_activeSession.store(session, std::memory_order_release);
        g_active.store(true, std::memory_order_release);
        publish_pose_locked();
        g_failure.store(Failure::none, std::memory_order_release);
        report("activate", "ok");
    }

    const std::uint32_t playerIndex = g_framePlayer.load(std::memory_order_acquire);
    if (playerIndex == kInvalidPlayer || playerIndex != g_state.playerIndex) {
        g_state.applied = false;
        ReleaseSRWLockExclusive(&g_stateLock);
        g_failure.store(Failure::playerChanged, std::memory_order_release);
        g_requested.store(false, std::memory_order_release);
        return nativeValid;
    }

    const Pose output = g_state.pose;
    g_state.applied = true;
    ++g_state.generation;
    publish_pose_locked();
    ReleaseSRWLockExclusive(&g_stateLock);
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        position[lane] = output.position[lane];
        forward[lane] = output.forward[lane];
        up[lane] = output.up[lane];
    }
    return true;
}

[[nodiscard]] bool read_bindings() noexcept {
    if (g_bindingsRead) {
        return true;
    }
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    for (std::size_t index = 0; index < kMovementActions.size(); ++index) {
        g_bindings[index] =
            account.settings.keyBindings.values[static_cast<std::size_t>(kMovementActions[index])];
    }
    g_bindingsRead = true;
    return true;
}

[[nodiscard]] bool half_down(const std::optional<std::uint16_t>& half) noexcept {
    if (!half.has_value()) {
        return false;
    }
    const std::uint32_t key = hooks::teleport::action_key(*half);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0;
}

[[nodiscard]] bool action_down(std::size_t index) noexcept {
    const bindings::Binding& binding = g_bindings[index];
    return half_down(binding.primary) || half_down(binding.secondary);
}

[[nodiscard]] bool publish_claimed_keys(const client::viewer::Settings& settings) noexcept {
    if (!read_bindings() || !hooks::teleport::action_keys_ready()) {
        std::array<std::uint32_t, 256> allKeys{};
        for (std::size_t key = 0; key < allKeys.size(); ++key) {
            allKeys[key] = static_cast<std::uint32_t>(key);
        }
        hooks::polled_input::claim_keys(allKeys);
        return false;
    }

    std::array<std::uint32_t, kMovementActions.size() * 2 + 3> keys{};
    std::size_t count = 0;
    for (const bindings::Binding& binding : g_bindings) {
        for (const std::optional<std::uint16_t>& half : {binding.primary, binding.secondary}) {
            if (!half.has_value()) {
                continue;
            }
            const std::uint32_t key = hooks::teleport::action_key(*half);
            if (key != 0 && count < keys.size()) {
                keys[count++] = key;
            }
        }
    }
    if (settings.toggleKey != kNoKey) {
        keys[count++] = settings.toggleKey;
    }
    keys[count++] = VK_SHIFT;
    keys[count++] = VK_CONTROL;
    hooks::polled_input::claim_keys(std::span(keys.data(), count));
    return true;
}

void coordinate_presentation(const client::viewer::Settings& settings) noexcept {
    if (!settings.hideWeaponOnEnter && !settings.removeHudOnEnter) {
        return;
    }
    const client::player::Settings before = client::player::get();
    client::player::Settings applied = before;
    if (settings.hideWeaponOnEnter) {
        applied.hideWeapon = true;
    }
    if (settings.removeHudOnEnter) {
        applied.removeHud = true;
    }
    if ((applied.hideWeapon != before.hideWeapon || applied.removeHud != before.removeHud)
        && !client::player::publish(applied)) {
        return;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.presentation = PresentationSession{
        before,
        applied,
        settings.hideWeaponOnEnter && !before.hideWeapon,
        settings.removeHudOnEnter && !before.removeHud,
        true,
    };
    ReleaseSRWLockExclusive(&g_stateLock);
}

void restore_presentation() noexcept {
    AcquireSRWLockExclusive(&g_stateLock);
    const PresentationSession session = g_state.presentation;
    g_state.presentation = {};
    ReleaseSRWLockExclusive(&g_stateLock);
    if (!session.active) {
        return;
    }
    client::player::Settings current = client::player::get();
    bool changed = false;
    if (session.hideWeaponOwned && current.hideWeapon == session.applied.hideWeapon) {
        current.hideWeapon = session.before.hideWeapon;
        changed = true;
    }
    if (session.removeHudOwned && current.removeHud == session.applied.removeHud) {
        current.removeHud = session.before.removeHud;
        changed = true;
    }
    if (changed) {
        (void)client::player::publish(current);
    }
}

void stop_runtime(Failure failure) noexcept {
    AcquireSRWLockExclusive(&g_stateLock);
    const bool wasActive = g_state.active;
    g_state.active = false;
    g_state.applied = false;
    g_state.playerIndex = kInvalidPlayer;
    g_state.controlledHandle = kInvalidPlayer;
    g_state.cameraIdentity = {};
    g_state.activeSession = 0;
    g_state.lastTick = 0;
    clear_playback_locked();
    ++g_state.generation;
    publish_pose_locked();
    ReleaseSRWLockExclusive(&g_stateLock);
    g_activeSession.store(0, std::memory_order_release);
    g_active.store(false, std::memory_order_release);
    g_mouseX.store(0, std::memory_order_release);
    g_mouseY.store(0, std::memory_order_release);
    hooks::polled_input::clear_claimed_keys();
    workspace_input::reset();
    if (failure != Failure::none) {
        g_failure.store(failure, std::memory_order_release);
    }
    if (wasActive) {
        report("deactivate", "ok");
    }
}

void finish_exit(Failure failure = Failure::none) noexcept {
    g_requested.store(false, std::memory_order_release);
    stop_runtime(failure);
    restore_presentation();
    AcquireSRWLockExclusive(&g_stateLock);
    g_state.transitioned = false;
    ReleaseSRWLockExclusive(&g_stateLock);
}

void update_toggle(const client::viewer::Settings& settings, bool uiVisible) noexcept {
    const bool down =
        settings.toggleKey != kNoKey && window_input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.toggleKey)) & kKeyHeldBit) != 0;
    AcquireSRWLockExclusive(&g_stateLock);
    const bool pressed = down && !g_state.toggleDown;
    g_state.toggleDown = down;
    ReleaseSRWLockExclusive(&g_stateLock);
    if (pressed && !uiVisible) {
        g_requested.store(!g_requested.load(std::memory_order_acquire), std::memory_order_release);
    }
}

void update_motion(const client::viewer::Settings& settings, bool uiVisible) noexcept {
    const std::uint64_t now = GetTickCount64();
    const long mouseX = g_mouseX.exchange(0, std::memory_order_acq_rel);
    const long mouseY = g_mouseY.exchange(0, std::memory_order_acq_rel);

    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_state.active) {
        ReleaseSRWLockExclusive(&g_stateLock);
        return;
    }
    const std::uint64_t elapsed =
        g_state.lastTick == 0 ? 0 : std::min(now - g_state.lastTick, kMaximumFrameMilliseconds);
    g_state.lastTick = now;
    bool playbackStarted = false;
    bool playbackScrubbed = false;
    if (g_state.playbackStopRequested) {
        clear_playback_locked();
    }
    if (g_state.playbackStartRequested) {
        g_state.playbackStartRequested = false;
        g_state.playbackElapsed = 0.0F;
        g_state.playbackKeyframe = 0;
        g_state.playbackPlaying = true;
        g_state.playbackPaused = false;
        playbackStarted = true;
    }
    if (g_state.playbackScrub >= 0.0F && g_state.playbackPlaying) {
        g_state.playbackElapsed = std::clamp(g_state.playbackScrub, 0.0F,
                                             playback_duration(g_state.playback));
        g_state.playbackScrub = -1.0F;
        playbackScrubbed = true;
    }

    bool movementDown = false;
    if (read_bindings()) {
        for (std::size_t index = 0; index < kMovementActions.size(); ++index) {
            movementDown = movementDown || action_down(index);
        }
    }
    if (g_state.playbackPlaying && (mouseX != 0 || mouseY != 0 || movementDown)) {
        clear_playback_locked();
    }
    if (g_state.playbackPlaying) {
        if (!window_input::game_focused()) {
            ReleaseSRWLockExclusive(&g_stateLock);
            return;
        }
        const float duration = playback_duration(g_state.playback);
        const float previousElapsed = g_state.playbackElapsed;
        if (!g_state.playbackPaused) {
            g_state.playbackElapsed += static_cast<float>(elapsed) / 1000.0F;
        }
        bool wrapped = false;
        if (g_state.playbackElapsed >= duration) {
            if (g_state.playback.loop && duration > 0.0F) {
                g_state.playbackElapsed = std::fmod(g_state.playbackElapsed, duration);
                wrapped = true;
            } else {
                g_state.playbackElapsed = duration;
            }
        }
        g_state.pose = playback_pose(g_state.playback, g_state.playbackElapsed,
                                     g_state.playbackKeyframe);
        if (!playbackScrubbed) {
            if (wrapped) {
                enqueue_capture_range(g_state.playback, previousElapsed, duration,
                                      playbackStarted);
                enqueue_capture_range(g_state.playback, -1.0F, g_state.playbackElapsed, true);
            } else {
                enqueue_capture_range(g_state.playback, previousElapsed, g_state.playbackElapsed,
                                      playbackStarted);
            }
        }
        g_playbackFov.store(g_state.pose.fov, std::memory_order_release);
        g_playbackFovActive.store(true, std::memory_order_release);
        publish_pose_locked();
        const bool finished = !g_state.playback.loop && g_state.playbackElapsed >= duration;
        if (finished) {
            g_state.playbackPlaying = false;
            g_state.playbackPaused = false;
            g_playbackFovActive.store(false, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&g_stateLock);
        return;
    }
    if ((uiVisible && !workspace_input::workspace_navigation())
        || !window_input::game_focused()) {
        ReleaseSRWLockExclusive(&g_stateLock);
        return;
    }

    g_state.pose.yaw -= static_cast<float>(mouseX) * settings.mouseSensitivity;
    g_state.pose.pitch =
        std::clamp(g_state.pose.pitch - static_cast<float>(mouseY) * settings.mouseSensitivity,
                   -kPitchLimit,
                   kPitchLimit);
    rebuild_basis(g_state.pose);

    Vector movement{};
    if (read_bindings()) {
        const Vector right = right_of(g_state.pose.forward);
        const auto add = [&movement](const Vector& axis, float scale) noexcept {
            for (std::size_t lane = 0; lane < movement.size(); ++lane) {
                movement[lane] += axis[lane] * scale;
            }
        };
        if (action_down(0)) {
            add(g_state.pose.forward, 1.0F);
        }
        if (action_down(1)) {
            add(g_state.pose.forward, -1.0F);
        }
        if (action_down(2)) {
            add(right, -1.0F);
        }
        if (action_down(3)) {
            add(right, 1.0F);
        }
        if (action_down(4)) {
            movement[2] += 1.0F;
        }
        if (action_down(5) || action_down(6)) {
            movement[2] -= 1.0F;
        }
    }
    if (normalize(movement)) {
        float speed = settings.speed;
        if ((GetAsyncKeyState(VK_SHIFT) & kKeyHeldBit) != 0) {
            speed *= settings.boostMultiplier;
        }
        if ((GetAsyncKeyState(VK_CONTROL) & kKeyHeldBit) != 0) {
            speed *= settings.precisionMultiplier;
        }
        const float distance = speed * static_cast<float>(elapsed) / 1000.0F;
        for (std::size_t lane = 0; lane < movement.size(); ++lane) {
            g_state.pose.position[lane] += movement[lane] * distance;
        }
    }
    g_state.pose.fov = g_outputFov.load(std::memory_order_acquire);
    publish_pose_locked();
    ReleaseSRWLockExclusive(&g_stateLock);
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    g_stopping.store(false, std::memory_order_release);
    g_installPublishing.store(true, std::memory_order_release);
    std::byte* const fov = patterns::scan_main_image_unique(kFovCopy, "viewer_camera_fov_copy");
    if (fov == nullptr) {
        g_failure.store(Failure::fovSignature, std::memory_order_release);
        g_installPublishing.store(false, std::memory_order_release);
        report("install", "fail", "fov_signature");
        return false;
    }
    std::byte* const pose = patterns::scan_main_image_unique(kPoseCopy, "viewer_camera_pose_copy");
    if (pose == nullptr) {
        g_failure.store(Failure::poseSignature, std::memory_order_release);
        g_installPublishing.store(false, std::memory_order_release);
        report("install", "fail", "pose_signature");
        return false;
    }
    const std::array specs{
        hooking::detour::Spec{fov, reinterpret_cast<void*>(&copy_fov)},
        hooking::detour::Spec{pose, reinterpret_cast<void*>(&copy_pose)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        g_failure.store(Failure::detourAttach, std::memory_order_release);
        g_installPublishing.store(false, std::memory_order_release);
        report("install", "fail", "detour_attach");
        return false;
    }
    g_originalFov.store(reinterpret_cast<FovCopy>(g_handles[kFovSlot].original),
                        std::memory_order_release);
    g_originalPose.store(reinterpret_cast<PoseCopy>(g_handles[kPoseSlot].original),
                         std::memory_order_release);
    g_failure.store(Failure::none, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    g_installPublishing.store(false, std::memory_order_release);
    report("install", "ok");
    return true;
}

bool uninstall() noexcept {
    g_stopping.store(true, std::memory_order_release);
    finish_exit();
    if (g_handles[kFovSlot].attached || g_handles[kPoseSlot].attached) {
        const std::array protectedEntries{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&copy_fov)},
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&copy_pose)},
        };
        const hooking::detour::UninstallResult removal =
            hooking::detour::uninstall(g_handles, protectedEntries, &replacements_idle);
        if (removal != hooking::detour::UninstallResult::removed) {
            g_failure.store(Failure::detourDetach, std::memory_order_release);
            report("uninstall",
                   removal == hooking::detour::UninstallResult::protectedCodeActive ? "wait"
                                                                                    : "fail",
                   removal == hooking::detour::UninstallResult::protectedCodeActive
                       ? "replacement_active"
                       : "detour_detach");
            return false;
        }
    }
    g_handles = {};
    g_originalFov.store(nullptr, std::memory_order_release);
    g_originalPose.store(nullptr, std::memory_order_release);
    g_installed.store(false, std::memory_order_release);
    g_bindings = {};
    g_bindingsRead = false;
    AcquireSRWLockExclusive(&g_captureLock);
    g_captureRequests = {};
    g_captureHead = 0;
    g_captureCount = 0;
    ReleaseSRWLockExclusive(&g_captureLock);
    report("uninstall", "ok");
    return true;
}

void poll(std::uint32_t playerIndex) noexcept {
    g_framePlayer.store(playerIndex, std::memory_order_release);
    if (!g_installed.load(std::memory_order_acquire)
        || g_stopping.load(std::memory_order_acquire)) {
        return;
    }
    const client::viewer::Settings settings = client::viewer::get();
    g_configuredFov.store(settings.fov, std::memory_order_release);
    const bool uiVisible = core::ui::runtime::snapshot().visible;
    update_toggle(settings, uiVisible);

    const bool requested = g_requested.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_stateLock);
    const bool transitioned = g_state.transitioned;
    if (requested != transitioned) {
        g_state.transitioned = requested;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
    if (requested && !transitioned) {
        coordinate_presentation(settings);
    } else if (!requested && transitioned) {
        finish_exit();
        return;
    }
    if (!requested) {
        return;
    }
    std::uint32_t controlledHandle = kInvalidPlayer;
    if (playerIndex == kInvalidPlayer
        || !hooks::teleport::controlled_player_handle(controlledHandle)) {
        finish_exit(Failure::localPlayer);
        return;
    }
    hooks::teleport::CameraIdentity cameraIdentity{};
    if (!hooks::teleport::camera_identity(playerIndex, cameraIdentity)) {
        finish_exit(Failure::cameraTarget);
        return;
    }
    AcquireSRWLockShared(&g_stateLock);
    const bool wrongPlayer =
        g_state.active
        && (g_state.playerIndex != playerIndex || g_state.controlledHandle != controlledHandle
            || g_state.cameraIdentity.sourceHandle != cameraIdentity.sourceHandle
            || g_state.cameraIdentity.sourceClass != cameraIdentity.sourceClass);
    ReleaseSRWLockShared(&g_stateLock);
    if (wrongPlayer) {
        finish_exit(Failure::playerChanged);
        return;
    }
    AcquireSRWLockShared(&g_stateLock);
    const PlaybackPath playback = g_state.playback;
    const bool playbackPending = g_state.playbackPlaying || g_state.playbackStartRequested;
    ReleaseSRWLockShared(&g_stateLock);
    if (playbackPending && playback.activitySession != 0) {
        state::activity::SessionSnapshot session{};
        if (!state::activity::snapshot_session(playback.activitySession, session)
            || session.binding.createdRevision != playback.activityRevision) {
            request_playback_stop();
        }
    }
    const bool inputReady = publish_claimed_keys(settings);
    if (!inputReady) {
        g_failure.store(Failure::inputBindings, std::memory_order_release);
    } else if (g_failure.load(std::memory_order_acquire) == Failure::inputBindings) {
        g_failure.store(Failure::none, std::memory_order_release);
    }
    update_motion(settings, uiVisible);
}

void add_mouse_delta(long x, long y) noexcept {
    if (!captures_mouse()) {
        return;
    }
    g_mouseX.fetch_add(x, std::memory_order_relaxed);
    g_mouseY.fetch_add(y, std::memory_order_relaxed);
}

bool captures_mouse() noexcept {
    return g_installed.load(std::memory_order_acquire)
           && g_requested.load(std::memory_order_acquire)
           && !g_stopping.load(std::memory_order_acquire)
           && (!core::ui::runtime::snapshot().visible
               || workspace_input::workspace_navigation())
           && window_input::game_focused();
}

void request_active(bool active) noexcept {
    if (!active || g_installed.load(std::memory_order_acquire)) {
        g_requested.store(active, std::memory_order_release);
    }
}

bool move_to(const Pose& pose) noexcept {
    if (!finite(pose.position) || !std::isfinite(pose.yaw) || !std::isfinite(pose.pitch)
        || !std::isfinite(pose.fov)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_state.active) {
        ReleaseSRWLockExclusive(&g_stateLock);
        return false;
    }
    clear_playback_locked();
    g_state.pose.position = pose.position;
    g_state.pose.yaw = pose.yaw;
    g_state.pose.pitch = std::clamp(pose.pitch, -kPitchLimit, kPitchLimit);
    rebuild_basis(g_state.pose);
    ++g_state.generation;
    publish_pose_locked();
    ReleaseSRWLockExclusive(&g_stateLock);
    return true;
}

bool return_to_player() noexcept {
    const client::player::position::Snapshot player = client::player::position::snapshot();
    if (!player.present || !finite(player.position)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_state.active) {
        ReleaseSRWLockExclusive(&g_stateLock);
        return false;
    }
    clear_playback_locked();
    g_state.pose.position = player.position;
    ++g_state.generation;
    publish_pose_locked();
    ReleaseSRWLockExclusive(&g_stateLock);
    return true;
}

bool requested() noexcept {
    return g_requested.load(std::memory_order_acquire);
}

bool active() noexcept {
    return g_active.load(std::memory_order_acquire);
}

std::uint64_t active_session() noexcept {
    return g_activeSession.load(std::memory_order_acquire);
}

bool pose_snapshot(Pose& pose) noexcept {
    if (!g_posePresent.load(std::memory_order_acquire)) {
        return false;
    }
    for (;;) {
        const std::uint32_t before = g_poseSequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        pose = g_publishedPose;
        if (g_poseSequence.load(std::memory_order_acquire) == before) {
            break;
        }
    }
    return g_posePresent.load(std::memory_order_acquire);
}

Status status() noexcept {
    Status snapshot{};
    AcquireSRWLockShared(&g_stateLock);
    snapshot.pose = g_state.pose;
    snapshot.pose.fov = g_playbackFovActive.load(std::memory_order_acquire)
                            ? g_playbackFov.load(std::memory_order_acquire)
                            : g_outputFov.load(std::memory_order_acquire);
    snapshot.generation = g_state.generation;
    snapshot.activeSession = g_state.activeSession;
    snapshot.active = g_state.active;
    snapshot.applied = g_state.applied;
    ReleaseSRWLockShared(&g_stateLock);
    snapshot.failure = g_failure.load(std::memory_order_acquire);
    snapshot.installed = g_installed.load(std::memory_order_acquire);
    snapshot.requested = g_requested.load(std::memory_order_acquire);
    return snapshot;
}

bool request_playback(const PlaybackPath& path) noexcept {
    if (!valid_playback(path)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    if (!g_state.active) {
        ReleaseSRWLockExclusive(&g_stateLock);
        return false;
    }
    g_state.playback = path;
    g_state.playbackElapsed = 0.0F;
    g_state.playbackScrub = -1.0F;
    g_state.playbackStartRequested = true;
    g_state.playbackStopRequested = false;
    ReleaseSRWLockExclusive(&g_stateLock);
    return true;
}

void request_playback_pause(bool paused) noexcept {
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_state.playbackPlaying) {
        g_state.playbackPaused = paused;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

void request_playback_stop() noexcept {
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_state.playbackPlaying || g_state.playbackStartRequested) {
        g_state.playbackStopRequested = true;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

void request_playback_scrub(float seconds) noexcept {
    if (!std::isfinite(seconds)) {
        return;
    }
    AcquireSRWLockExclusive(&g_stateLock);
    if (g_state.playbackPlaying) {
        g_state.playbackScrub = seconds;
    }
    ReleaseSRWLockExclusive(&g_stateLock);
}

PlaybackStatus playback_status() noexcept {
    PlaybackStatus result{};
    AcquireSRWLockShared(&g_stateLock);
    result.elapsedSeconds = g_state.playbackElapsed;
    result.durationSeconds = playback_duration(g_state.playback);
    result.keyframeIndex = g_state.playbackKeyframe;
    result.playing = g_state.playbackPlaying || g_state.playbackStartRequested;
    result.paused = g_state.playbackPaused;
    ReleaseSRWLockShared(&g_stateLock);
    return result;
}

bool consume_snapshot_capture_request(SnapshotCaptureRequest& request) noexcept {
    AcquireSRWLockExclusive(&g_captureLock);
    if (g_captureCount == 0) {
        ReleaseSRWLockExclusive(&g_captureLock);
        request = {};
        return false;
    }
    request = g_captureRequests[g_captureHead];
    g_captureHead = (g_captureHead + 1U) % g_captureRequests.size();
    --g_captureCount;
    ReleaseSRWLockExclusive(&g_captureLock);
    return true;
}

const char* failure_name(Failure failure) noexcept {
    switch (failure) {
    case Failure::none:
        return "none";
    case Failure::poseSignature:
        return "pose signature unavailable";
    case Failure::fovSignature:
        return "FOV signature unavailable";
    case Failure::detourAttach:
        return "camera detour attach failed";
    case Failure::nativePose:
        return "native camera pose invalid";
    case Failure::inputBindings:
        return "movement bindings unavailable";
    case Failure::localPlayer:
        return "local player unavailable";
    case Failure::cameraTarget:
        return "native camera target unavailable";
    case Failure::playerChanged:
        return "camera owner changed";
    case Failure::detourDetach:
        return "camera detour still active";
    }
    return "unknown";
}

} // namespace sunrise::client::viewer::camera
