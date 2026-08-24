#pragma once

#include <Windows.h>

#include <cstddef>
#include <d3d11.h>
#include <dxgi.h>

#include "../textures/graphics_texture_upload.h"
#include "graphics_debug_render.h"
#include "graphics_frame_capture.h"

namespace sunrise::client::hooks::graphics::renderer {

/** The SDK objects we own and the started presentation layers, for one swap chain. */
struct Resources {
    IDXGISwapChain* swapChain{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11RenderTargetView* renderTarget{};
    /** Bundled logo sheet, uploaded on this device for the interface to draw. */
    textures::Uploaded logoSheet{};
    /** Compact project icon used by the inspector workspace toolbar. */
    textures::Uploaded inspectorIcon{};
    /** Reusable game-frame copy shown inside the inspector viewport. */
    frame_capture::Storage frameCapture{};
    /** Depth-tested in-scene helper pass state. */
    debug_render::Storage debugRender{};
    HWND window{};
    bool layoutInitialized{};
    bool win32BackendInitialized{};
    bool dx11BackendInitialized{};
    bool inputInstalled{};
    bool inputVisible{};
    bool surfaceChangeDeviceLost{};
    std::size_t activeSurfaceChanges{};
};

extern SRWLOCK g_rendererLock;
extern Resources g_resources;
/** Window whose capture release waits until every renderer and hook lock is gone. */
extern HWND g_captureReleaseWindow;

namespace selection {

/**
 * Acquires and validates renderer resources for one swap chain.
 * @param swapChain Candidate
 * game swap chain.
 * @param output Destination for Sunrise-owned references and selected
 * identities.
 * @return True when the swap chain and required D3D objects were selected.
 */
[[nodiscard]] bool acquire(IDXGISwapChain* swapChain, Resources& output) noexcept;

/**
 * Compares a swap chain's output window with a previously selected window.
 * @param swapChain
 * Candidate game swap chain.
 * @param window Expected output window.
 * @return True when the
 * current swap-chain descriptor names the expected window.
 */
[[nodiscard]] bool matches_output_window(IDXGISwapChain* swapChain, HWND window) noexcept;

} // namespace selection

/** @param resources Chosen SDK resources. @return True when a back-buffer RTV is owned. */
[[nodiscard]] bool create_render_target(Resources& resources) noexcept;

/** @param resources Chosen resources whose RTV is unbound and released. */
void release_render_target(Resources& resources) noexcept;

/** @param resources SDK resources freed in an order that respects their dependencies. */
void release_resources(Resources& resources) noexcept;

/** @return True when every layer is shut down and cleared. */
[[nodiscard]] bool shutdown_locked() noexcept;

/** @return True when the whole presentation stack is ready for frames and input. */
[[nodiscard]] bool fully_active_locked() noexcept;

/** Runs one Dear ImGui frame. Draws only while the renderer lock is held and UI is visible. */
void render_frame_locked() noexcept;

} // namespace sunrise::client::hooks::graphics::renderer
