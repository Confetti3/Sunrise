#pragma once

#include <Windows.h>

#include <dxgi.h>

#include "graphics_frame_capture.h"

namespace sunrise::client::hooks::graphics::renderer {

/**
 * Draws the UI frame, if any, for a checked swap chain.
 * @param swapChain Borrowed swap chain
 * supplied by the Present replacement.
 */
void present(IDXGISwapChain* swapChain) noexcept;

/**
 * Drops our render target before a call that recreates the back buffers.
 * @param swapChain
 * Borrowed swap chain whose surface is changing.
 */
void before_surface_change(IDXGISwapChain* swapChain) noexcept;

/**
 * Rebuilds our render state after a surface-changing call returns.
 * @param swapChain Borrowed
 * swap chain whose surface changed.
 * @param result HRESULT returned by the original
 * surface-changing call.
 */
void after_surface_change(IDXGISwapChain* swapChain, HRESULT result) noexcept;

/**
 * Frees Sunrise-owned resources when Present reports a lost device.
 * @param swapChain
 * Borrowed swap chain used by the completed Present call.
 * @param result HRESULT returned by the
 * original Present call.
 */
void present_result(IDXGISwapChain* swapChain, HRESULT result) noexcept;

/** @return True only after every renderer and window resource is freed. */
[[nodiscard]] bool shutdown() noexcept;

/** @return True once a chosen swap chain has started all UI resources. */
[[nodiscard]] bool active() noexcept;

/** @return Current captured game frame. Call only from the renderer-owned UI frame. */
[[nodiscard]] frame_capture::View captured_frame_locked() noexcept;

/**
 * Feeds one window message into the live Dear ImGui context.
 * @param window Window receiving the
 * message.
 * @param message Win32 message ID.
 * @param word Message-specific WPARAM value.
 *
 * @param value Message-specific LPARAM value.
 * @return True for every mouse, keyboard and
 * raw-input message while the UI is visible.
 */
[[nodiscard]] bool
handle_window_message(HWND window, UINT message, WPARAM word, LPARAM value) noexcept;

/** @param window Live output window whose deferred capture release may run. */
void dispatch_pending_input_release(HWND window) noexcept;

/** Runs any deferred capture release once the hook and renderer locks are gone. */
void dispatch_pending_input_release() noexcept;

} // namespace sunrise::client::hooks::graphics::renderer
