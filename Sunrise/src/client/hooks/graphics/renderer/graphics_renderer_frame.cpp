#include <Windows.h>

#include <array>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "../../../../core/logging/log.h"
#include "../../../../core/ui/busy/busy.h"
#include "../../../../core/ui/fonts/runtime/ui_runtime_font_lifecycle.h"
#include "../../../../core/ui/hud/overlay.h"
#include "../../../../core/ui/layout/layout.h"
#include "../../../../core/ui/notice/ui_notice_overlay.h"
#include "../../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../../core/ui/scaling/dpi/ui_dpi_scaling.h"
#include "../../../../core/ui/theme/sunrise_ui_theme.h"
#include "../../../inspection/inspection_workspace_host.h"
#include "../../viewer_camera/viewer_camera.h"
#include "../input/input.h"
#include "graphics_debug_render.h"
#include "graphics_depth_observer.h"
#include "graphics_renderer_report.h"
#include "native_debug_renderer.h"
#include "state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window,
                                                             UINT message,
                                                             WPARAM word,
                                                             LPARAM value);

namespace sunrise::client::hooks::graphics::renderer {
namespace {

/** The fitted inspector clears every back-buffer pixel before its captured frame is composited. */
constexpr std::array<float, 4> kInspectorClearColor{0.025F, 0.029F, 0.035F, 1.0F};
/** The overlay uses one render target, set only while Dear ImGui draws. */
constexpr UINT kOverlayRenderTargetCount = 1;
// A visible UI eats this whole range. A range that stopped short of the wheel or the extra
// buttons would leak them to the game with no other sign.
static_assert(WM_MOUSEWHEEL <= WM_MOUSELAST);
static_assert(WM_MOUSEHWHEEL <= WM_MOUSELAST);
static_assert(WM_XBUTTONDBLCLK <= WM_MOUSELAST);

[[nodiscard]] bool output_surface_usable(HWND window) noexcept {
    if (window == nullptr || IsIconic(window) != FALSE) {
        return false;
    }
    RECT client{};
    return GetClientRect(window, &client) != FALSE && client.right > client.left
           && client.bottom > client.top;
}

/** @param message Win32 message ID. @return True for ordinary client-area mouse input. */
[[nodiscard]] bool is_mouse_input(UINT message) noexcept {
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
}

/** @param message Win32 message ID. @return True for the raw mouse and keyboard delivery. */
[[nodiscard]] bool is_raw_input(UINT message) noexcept {
    return message == WM_INPUT;
}

/**
 * Says which keyboard and text messages the UI bridge takes.
 * @param message Win32 message ID.
 * @return True for ordinary keyboard or text input.
 */
[[nodiscard]] bool is_keyboard_input(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return true;
    default:
        return false;
    }
}

/**
 * Applies a visibility change to Dear ImGui input and drops events queued while hidden.
 * @param visible Current Core visibility state.
 */
void transition_input_visibility_locked(bool visible) noexcept {
    ImGuiIO& io = ImGui::GetIO();
    // The game renders a virtual cursor, so visible Sunrise frames need ImGui's software cursor.
    io.MouseDrawCursor = visible;
    if (visible) {
        g_resources.inputVisible = true;
        return;
    }
    if (!g_resources.inputVisible) {
        return;
    }

    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    g_resources.inputVisible = false;
    // The capture release is deferred because ReleaseCapture can re-enter WndProc at once.
    g_captureReleaseWindow = g_resources.window;
}

/**
 * Draws the inspector's world helpers into the live game frame with depth-tested
 * occlusion, when the Inspector is visible, the detached camera is driving the view,
 * and a scene
 * depth copy was captured this frame. A miss is published as an explicit
 * failure; the Inspector
 * does not substitute unoccluded geometry.
 */
void draw_scene_helpers_locked(const client::viewer::camera::Status& status,
                               const frame_capture::View& frame,
                               const client::inspection::RenderViewSnapshot& exactView) noexcept {
    namespace workspace = client::inspection::workspace_host;
    ID3D11RenderTargetView* target = frame_capture::render_target(g_resources.frameCapture);
    const client::inspection::SceneFramePtr scene = workspace::scene_frame();
    client::inspection::DepthRenderStatus depthStatus{};
    depthStatus.capturedFrame = frame.frameId;
    depthStatus.failureReason = client::inspection::HelperFailureReason::depthUnavailable;
    depthStatus.capability = g_resources.debugRender.capability;
    depthStatus.depthAvailable = debug_render::depth_available(g_resources.debugRender);
    depthStatus.depthSequence = g_resources.debugRender.captureSequence;
    depthStatus.depthCaptureMicros = g_resources.debugRender.lastCaptureMicros;
    if (scene) {
        depthStatus.batchSequence = scene->sequence;
        depthStatus.graphGeneration = scene->graphGeneration;
    }
    const auto publish = [&]() noexcept {
        static client::inspection::HelperBackend lastBackend{
            client::inspection::HelperBackend::none};
        static client::inspection::HelperFailureReason lastReason{
            client::inspection::HelperFailureReason::none};
        if (depthStatus.backend != lastBackend || depthStatus.failureReason != lastReason) {
            std::array<char, 224> event{};
            const int written =
                std::snprintf(event.data(),
                              event.size(),
                              "ev=renderer stage=scene_helpers backend=%s failure=%s frame=%llu",
                              client::inspection::helper_backend_name(depthStatus.backend),
                              client::inspection::helper_failure_name(depthStatus.failureReason),
                              static_cast<unsigned long long>(depthStatus.capturedFrame));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::debug,
                                 {event.data(), static_cast<std::size_t>(written)});
            }
            lastBackend = depthStatus.backend;
            lastReason = depthStatus.failureReason;
        }
        workspace::publish_depth_status(depthStatus);
    };
    if (!workspace::depth_helpers_enabled() || !frame || !scene) {
        publish();
        return;
    }
    depthStatus.engineFrame = exactView.engineFrame;
    depthStatus.viewPublication = exactView.publication;
    if (!exactView.valid || !exactView.exactNative || exactView.engineFrame == 0
        || exactView.publication == 0 || exactView.viewport.width <= 0.0F
        || exactView.viewport.height <= 0.0F) {
        depthStatus.failureReason = client::inspection::HelperFailureReason::exactViewUnavailable;
        publish();
        return;
    }
    if (!status.projectionAvailable || frame.height <= 0.0F || target == nullptr
        || !debug_render::depth_available(g_resources.debugRender)) {
        depthStatus.failureReason = client::inspection::HelperFailureReason::depthUnavailable;
        publish();
        return;
    }
    const std::size_t count = scene->lines.size();
    depthStatus.submittedLines = count;
    depthStatus.submittedGlyphs = scene->glyphs.size();
    if (count == 0 && scene->glyphs.empty()) {
        publish();
        return;
    }
    depthStatus.drawn = debug_render::draw_lines(g_resources.context,
                                                 target,
                                                 scene->lines.data(),
                                                 count,
                                                 g_resources.debugRender,
                                                 exactView,
                                                 scene->glyphs.data(),
                                                 scene->glyphs.size(),
                                                 scene->lineWidthPixels);
    if (depthStatus.drawn) {
        depthStatus.backend = client::inspection::HelperBackend::sunriseDepth;
        depthStatus.failureReason = client::inspection::HelperFailureReason::none;
        depthStatus.picking = debug_render::draw_pick_ids(
            g_resources.context, scene, frame.frameId, exactView, g_resources.debugRender);
    } else {
        depthStatus.failureReason = client::inspection::HelperFailureReason::drawFailed;
    }
    publish();
}

/**
 * Draws Dear ImGui data, then puts back every output-merger target that was set before.
 * @param drawData Completed frame draw data.
 * @param clearSurface True when the fitted inspector must hide the original full-screen frame.
 */
void draw_data(ImDrawData* drawData, bool clearSurface) noexcept {
    if (drawData == nullptr) {
        return;
    }
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> priorTargets{};
    ID3D11DepthStencilView* priorDepth = nullptr;
    g_resources.context->OMGetRenderTargets(
        static_cast<UINT>(priorTargets.size()), priorTargets.data(), &priorDepth);

    ID3D11RenderTargetView* overlayTarget = g_resources.renderTarget;
    g_resources.context->OMSetRenderTargets(kOverlayRenderTargetCount, &overlayTarget, nullptr);
    if (clearSurface) {
        g_resources.context->ClearRenderTargetView(overlayTarget, kInspectorClearColor.data());
    }
    ImGui_ImplDX11_RenderDrawData(drawData);
    g_resources.context->OMSetRenderTargets(
        static_cast<UINT>(priorTargets.size()), priorTargets.data(), priorDepth);

    for (ID3D11RenderTargetView* target : priorTargets) {
        if (target != nullptr) {
            target->Release();
        }
    }
    if (priorDepth != nullptr) {
        priorDepth->Release();
    }
}

} // namespace

/**
 * Runs one Dear ImGui frame. Draw data is sent only while the Core UI is visible.
 */
void render_frame_locked() noexcept {
    if (!fully_active_locked()) {
        return;
    }
    const core::ui::runtime::VisibilitySnapshot visibility = core::ui::runtime::snapshot();
    const bool inspectorSelected = client::inspection::workspace_host::visible();
    const bool inspectorCapturing = inspectorSelected && visibility.visible;
    const bool captureRequested =
        inspectorCapturing && client::inspection::workspace_host::depth_helpers_enabled();
    depth_observer::set_capture_requested(captureRequested);
    // Freeze game observations before Sunrise changes any output-merger state.
    const client::inspection::RenderViewSnapshot observedView = native_debug::view();
    const client::inspection::RenderViewSnapshot exactView =
        native_debug::view_current(observedView, GetTickCount64())
            ? observedView
            : client::inspection::RenderViewSnapshot{};
    depth_observer::sample_present(exactView);
    const depth_observer::Status observedDepth = depth_observer::status();
    depth_observer::CapturedDepth capturedDepth{};
    (void)depth_observer::take_latest(capturedDepth);
    // Everything below belongs to Sunrise's overlay pass. Its OM/depth calls use
    // the same vtable hooks and must not become proof or mailbox observations.
    const depth_observer::InternalCallScope internalD3dCalls{};
    static bool lastDepthEligible{};
    if (observedDepth.eligible != lastDepthEligible) {
        std::array<char, 256> event{};
        const int written = std::snprintf(event.data(),
                                          event.size(),
                                          "ev=renderer stage=depth_proof eligible=%u format=%u "
                                          "size=%ux%u samples=%u final=%u/%u matrices=%u reason=%s",
                                          observedDepth.eligible ? 1U : 0U,
                                          static_cast<unsigned>(observedDepth.descriptor.format),
                                          observedDepth.descriptor.width,
                                          observedDepth.descriptor.height,
                                          observedDepth.descriptor.sampleCount,
                                          observedDepth.qualifyingFrames,
                                          observedDepth.observedFrames,
                                          observedDepth.distinctMatrixHashes,
                                          depth_observer::failure_name(observedDepth.failure));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             observedDepth.eligible ? core::log::Level::info
                                                    : core::log::Level::warn,
                             {event.data(), static_cast<std::size_t>(written)});
        }
        lastDepthEligible = observedDepth.eligible;
    }
    if (!output_surface_usable(g_resources.window)) {
        depth_observer::release_captured(capturedDepth);
        depth_observer::set_capture_requested(false);
        frame_capture::release(g_resources.frameCapture);
        debug_render::release(g_resources.debugRender);
        client::inspection::workspace_host::suspend();
        transition_input_visibility_locked(false);
        return;
    }
    if (core::ui::scaling::dpi::update(g_resources.window)) {
        // Style and text scale change together, before the backend sets up the frame.
        core::ui::theme::apply();
        if (!core::ui::fonts::runtime::apply_scale(core::ui::scaling::dpi::current())) {
            report::note(report::Stage::frame, report::Reason::fontScale);
            depth_observer::release_captured(capturedDepth);
            (void)shutdown_locked();
            return;
        }
    }
    const client::viewer::camera::Status cameraStatus = client::viewer::camera::status();
    bool frameReady = false;
    if (inspectorCapturing) {
        frameReady = frame_capture::update(g_resources.device,
                                           g_resources.context,
                                           g_resources.swapChain,
                                           g_resources.frameCapture);
    }
    const bool sceneHelpersReady =
        inspectorCapturing && client::inspection::workspace_host::depth_helpers_enabled()
        && cameraStatus.projectionAvailable && frameReady
        && frame_capture::render_target(g_resources.frameCapture) != nullptr;
    if (sceneHelpersReady && observedDepth.captureEnabled && capturedDepth.view != nullptr) {
        // Borrowed only while Inspector owns the visible UI and a valid captured frame
        // is ready. The copied depth never remains live behind a stale session flag.
        debug_render::update_depth_from_view(g_resources.device,
                                             g_resources.context,
                                             capturedDepth.view,
                                             capturedDepth.convention
                                                 == depth_observer::Convention::reversed,
                                             capturedDepth.sequence,
                                             g_resources.debugRender);
    } else {
        debug_render::release(g_resources.debugRender);
    }
    depth_observer::release_captured(capturedDepth);
    if (inspectorCapturing) {
        // Select and complete the visual geometry backend before ImGui records this
        // captured frame. The viewport annotates only the exact frame that already
        // contains a successful depth/native draw.
        draw_scene_helpers_locked(
            cameraStatus, frame_capture::view(g_resources.frameCapture), exactView);
    }
    transition_input_visibility_locked(visibility.visible);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    client::inspection::workspace_host::service_camera_path_captures();
    // A hidden surface still draws until its close animation ends, so the layout decides. The
    // HUD, running-work and notice overlays draw whether the surface is open or not. The HUD
    // goes first, so the surface stays above it when the two meet.
    const bool suppressHud = inspectorSelected && visibility.visible;
    const bool hudDrawn = !suppressHud && core::ui::hud::draw(visibility.enabled);
    const bool surfaceDrawn = inspectorSelected
                                  ? client::inspection::workspace_host::render(visibility.visible)
                                  : core::ui::layout::render(visibility.visible);
    const bool busyDrawn = core::ui::busy::draw();
    const bool noticeDrawn = core::ui::notice::draw();
    if (!hudDrawn && !surfaceDrawn && !busyDrawn && !noticeDrawn) {
        // A frame nobody claimed still drains backend state, and sends no draw data.
        ImGui::EndFrame();
        return;
    }
    ImGui::Render();
    draw_data(ImGui::GetDrawData(), suppressHud);
}

frame_capture::View captured_frame_locked() noexcept {
    return frame_capture::view(g_resources.frameCapture);
}

/** Feeds one ordinary window message into the active Dear ImGui context. */
bool handle_window_message(HWND window, UINT message, WPARAM word, LPARAM value) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    if (!fully_active_locked() || g_resources.window != window) {
        ReleaseSRWLockExclusive(&g_rendererLock);
        return false;
    }

    const core::ui::runtime::VisibilitySnapshot visibility = core::ui::runtime::snapshot();
    transition_input_visibility_locked(visibility.visible);
    if (!visibility.visible) {
        // Hidden input stays with the game and never enters Dear ImGui's event queue.
        ReleaseSRWLockExclusive(&g_rendererLock);
        return false;
    }

    (void)ImGui_ImplWin32_WndProcHandler(window, message, word, value);
    // A visible UI is modal: no mouse, keyboard or raw input reaches the game, hit test aside.
    const bool capture =
        is_mouse_input(message) || is_keyboard_input(message) || is_raw_input(message);
    ReleaseSRWLockExclusive(&g_rendererLock);
    return capture;
}

/** Releases mouse capture, but only after the renderer and window locks are gone. */
void dispatch_pending_input_release(HWND window) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    const DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
    const bool releaseCapture = g_captureReleaseWindow == window && windowThread != 0
                                && windowThread == GetCurrentThreadId();
    if (releaseCapture) {
        g_captureReleaseWindow = nullptr;
    }
    ReleaseSRWLockExclusive(&g_rendererLock);

    if (releaseCapture && GetCapture() == window) {
        // The renderer lock is gone first, because Windows sends WM_CAPTURECHANGED at once.
        (void)ReleaseCapture();
    }
}

/** Runs any deferred capture release once the hook and renderer locks are gone. */
void dispatch_pending_input_release() noexcept {
    AcquireSRWLockShared(&g_rendererLock);
    const HWND window = g_captureReleaseWindow;
    ReleaseSRWLockShared(&g_rendererLock);

    if (window == nullptr) {
        return;
    }
    const DWORD windowThread = GetWindowThreadProcessId(window, nullptr);
    if (windowThread != 0 && windowThread != GetCurrentThreadId()) {
        // The game's presentation message still reaches hooked Present, even if we were unhooked.
        (void)PostMessageW(window, input::kRequiredForwardMessage, 0, 0);
        return;
    }
    dispatch_pending_input_release(window);
}

} // namespace sunrise::client::hooks::graphics::renderer
