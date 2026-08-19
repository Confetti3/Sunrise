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
    DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT viewFormat{DXGI_FORMAT_UNKNOWN};
    UINT width{};
    UINT height{};
    UINT sourceSamples{};
    bool descriptorRejected{};
};

/** Borrowed render-thread view of one captured game frame. */
struct View final {
    ImTextureID texture{ImTextureID_Invalid};
    float width{};
    float height{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return texture != ImTextureID_Invalid && width > 0.0F && height > 0.0F;
    }
};

/** Copies or resolves the current swap-chain buffer into reusable shader-resource storage. */
[[nodiscard]] bool update(ID3D11Device* device,
                          ID3D11DeviceContext* context,
                          IDXGISwapChain* swapChain,
                          Storage& storage) noexcept;

/** Releases all capture objects and descriptor state. */
void release(Storage& storage) noexcept;

/** Returns a borrowed ImGui texture view while the renderer lock is held. */
[[nodiscard]] View view(const Storage& storage) noexcept;

} // namespace sunrise::client::hooks::graphics::renderer::frame_capture
