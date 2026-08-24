#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>

namespace sunrise::client::hooks::graphics::renderer::frame_capture {

/** GPU-owned copy of the most recently captured game back buffer. */
struct Storage final {
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* view{};
    /** Optional RTV for drawing Inspector-only helpers into the captured copy. */
    ID3D11RenderTargetView* renderTarget{};
    DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT viewFormat{DXGI_FORMAT_UNKNOWN};
    UINT width{};
    UINT height{};
    UINT sourceSamples{};
    std::uint64_t frameId{};
    bool descriptorRejected{};
};

/** Borrowed render-thread view of one captured game frame. */
struct View final {
    ImTextureID texture{ImTextureID_Invalid};
    float width{};
    float height{};
    std::uint64_t frameId{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return texture != ImTextureID_Invalid && width > 0.0F && height > 0.0F;
    }
};

/**
 * Copies or resolves the current swap-chain buffer into reusable shader-resource storage.
 *
 * @param device Selected renderer-owned D3D device.
 * @param context Selected renderer-owned
 * immediate context.
 * @param swapChain Current game swap chain.
 * @param storage Sunrise-owned
 * reusable capture resources.
 * @return True when the current back buffer was captured.
 */
[[nodiscard]] bool update(ID3D11Device* device,
                          ID3D11DeviceContext* context,
                          IDXGISwapChain* swapChain,
                          Storage& storage) noexcept;

/**
 * Releases all capture objects and descriptor state.
 * @param storage Sunrise-owned capture
 * resources to release and clear.
 */
void release(Storage& storage) noexcept;

/**
 * Returns a borrowed ImGui texture view while the renderer lock is held.
 * @param storage
 * Capture storage protected by the renderer lock.
 * @return Borrowed texture handle, dimensions,
 * and frame identity.
 */
[[nodiscard]] View view(const Storage& storage) noexcept;

/**
 * Returns the optional captured-frame RTV while the renderer lock is held.
 * @param storage
 * Capture storage protected by the renderer lock.
 * @return Borrowed render-target view, or null
 * when none exists.
 */
[[nodiscard]] ID3D11RenderTargetView* render_target(const Storage& storage) noexcept;

} // namespace sunrise::client::hooks::graphics::renderer::frame_capture
