#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d11.h>

#include "../../../inspection/inspection_scene.h"

namespace sunrise::client::hooks::graphics::renderer::debug_render {

/**
 * Reports whether a D3D format belongs to a capturable depth family.
 * @param format Resource or view format to classify.
 * @return True for supported depth families.
 */
[[nodiscard]] constexpr bool depth_format_supported(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

/**
 * Applies the pure dimension, sample-count, and format capability gate.
 * @param width Depth resource width in pixels.
 * @param height Depth resource height in pixels.
 * @param samples Multisample count.
 * @param format Depth resource or view format.
 * @return Exact capability or rejection reason.
 */
[[nodiscard]] constexpr inspection::DepthCapability
evaluate_depth_descriptor(UINT width, UINT height, UINT samples, DXGI_FORMAT format) noexcept {
    if (width == 0 || height == 0) {
        return inspection::DepthCapability::invalidDimensions;
    }
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) {
        return inspection::DepthCapability::unsupportedSamples;
    }
    return depth_format_supported(format) ? inspection::DepthCapability::supported
                                          : inspection::DepthCapability::unsupportedFormat;
}

/** Inspector-side world-space line type. */
using SceneLine = ::sunrise::client::inspection::SceneLine;
using SceneGlyph = ::sunrise::client::inspection::SceneGlyph;

/**
 * GPU-side state for drawing depth-tested debug lines into the game's back buffer.
 * Everything here is owned by Sunrise and released on shutdown or device loss; no
 * engine object is retained beyond one borrowed frame.
 */
struct Storage final {
    /** Typeless-family copy of the game's depth-stencil, readable as a shader resource. */
    ID3D11Texture2D* depthTexture{};
    ID3D11ShaderResourceView* depthView{};
    ID3D11DepthStencilView* depthStencilView{};
    ID3D11Buffer* vertexBuffer{};
    ID3D11Buffer* glyphVertexBuffer{};
    ID3D11Buffer* constantBuffer{};
    ID3D11VertexShader* vertexShader{};
    ID3D11GeometryShader* lineGeometryShader{};
    ID3D11VertexShader* glyphVertexShader{};
    ID3D11GeometryShader* glyphGeometryShader{};
    ID3D11PixelShader* pixelShader{};
    ID3D11PixelShader* selectionHaloPixelShader{};
    ID3D11PixelShader* hoverPixelShader{};
    ID3D11InputLayout* inputLayout{};
    ID3D11InputLayout* glyphInputLayout{};
    ID3D11RasterizerState* lineRasterizerState{};
    ID3D11BlendState* blendState{};
    ID3D11DepthStencilState* standardDepthState{};
    ID3D11DepthStencilState* reversedDepthState{};
    ID3D11DepthStencilState* selectionHaloDepthState{};
    /** Private integer ID pass. It is never attached to the game's render targets. */
    ID3D11Texture2D* pickTexture{};
    ID3D11RenderTargetView* pickTarget{};
    ID3D11ShaderResourceView* pickView{};
    ID3D11Texture2D* pickDepthTexture{};
    ID3D11DepthStencilView* pickDepthStencilView{};
    ID3D11Texture2D* pickResolved{};
    ID3D11UnorderedAccessView* pickResolvedView{};
    ID3D11Buffer* pickVertexBuffer{};
    ID3D11Buffer* pickGlyphVertexBuffer{};
    ID3D11Buffer* pickConstantBuffer{};
    ID3D11Buffer* pickResolveConstantBuffer{};
    ID3D11VertexShader* pickVertexShader{};
    ID3D11GeometryShader* pickGeometryShader{};
    ID3D11VertexShader* pickGlyphVertexShader{};
    ID3D11GeometryShader* pickGlyphGeometryShader{};
    ID3D11PixelShader* pickPixelShader{};
    ID3D11ComputeShader* pickResolveShader{};
    ID3D11InputLayout* pickInputLayout{};
    ID3D11InputLayout* pickGlyphInputLayout{};
    ID3D11DepthStencilState* pickDepthState{};
    ID3D11RasterizerState* pickRasterizerState{};
    struct PickReadback final {
        ID3D11Texture2D* staging{};
        inspection::SceneFramePtr batch{};
        std::uint64_t requestSequence{};
        std::uint64_t capturedFrame{};
        std::uint64_t engineFrame{};
        std::uint64_t viewPublication{};
        std::uint64_t depthSequence{};
        std::uint32_t graphGeneration{};
        bool pending{};
    };
    std::array<PickReadback, 3> pickReadbacks{};
    std::size_t nextPickReadback{};
    std::uint64_t lastPickRequest{};
    std::uint64_t captureSequence{};
    D3D11_COMPARISON_FUNC depthComparison{D3D11_COMPARISON_LESS_EQUAL};
    DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN};
    UINT width{};
    UINT height{};
    UINT sourceSamples{};
    UINT sourceQuality{};
    std::uint32_t consecutiveFailures{};
    std::uint64_t lastCaptureMicros{};
    std::uint64_t maximumCaptureMicros{};
    std::uint64_t lastDrawMicros{};
    std::uint64_t maximumDrawMicros{};
    inspection::DepthCapability capability{inspection::DepthCapability::unavailable};
    bool slowCaptureReported{};
    bool slowDrawReported{};
    /** Set when the game's depth descriptor cannot be captured safely this session. */
    bool rejected{};
};

/**
 * Copies the currently bound game depth-stencil into SRV storage. An unsupported
 * descriptor leaves storage empty and disables helper geometry for that frame.
 * @param device Selected renderer-owned D3D device.
 * @param context Selected renderer-owned immediate context.
 * @param storage Sunrise-owned depth rendering resources.
 */
void update_depth(ID3D11Device* device, ID3D11DeviceContext* context, Storage& storage) noexcept;
/**
 * Copies one observer-proven depth view into Sunrise-owned shader resources.
 * @param device Selected renderer-owned D3D device.
 * @param context Selected renderer-owned immediate context.
 * @param source Borrowed proven depth-stencil view for this frame.
 * @param reversedZ True when the proven depth convention is reversed.
 * @param sourceSequence Monotonic observer capture identity.
 * @param storage Sunrise-owned depth rendering resources.
 */
void update_depth_from_view(ID3D11Device* device,
                            ID3D11DeviceContext* context,
                            ID3D11DepthStencilView* source,
                            bool reversedZ,
                            std::uint64_t sourceSequence,
                            Storage& storage) noexcept;

/**
 * Draws world-space inspector lines into @p target, testing each fragment against the
 * captured scene depth so helpers hide behind real geometry. Every pipeline state the
 * game had set is restored before the call returns.
 * @param context Selected renderer-owned immediate context.
 * @param target Borrowed target receiving Inspector helpers.
 * @param lines Pointer to a bounded line array, or null when lineCount is zero.
 * @param lineCount Number of line records.
 * @param storage Sunrise-owned reusable render resources.
 * @param exactView Proven exact native matrix and viewport for this frame.
 * @param glyphs Optional bounded glyph array.
 * @param glyphCount Number of glyph records.
 * @param lineWidthPixels Requested helper width in pixels.
 * @return True when the draw was submitted.
 */
[[nodiscard]] bool draw_lines(ID3D11DeviceContext* context,
                              ID3D11RenderTargetView* target,
                              const SceneLine* lines,
                              std::size_t lineCount,
                              Storage& storage,
                              const inspection::RenderViewSnapshot& exactView,
                              const SceneGlyph* glyphs = nullptr,
                              std::size_t glyphCount = 0,
                              float lineWidthPixels = 2.4F) noexcept;

/**
 * Draws sixteen-pixel helper IDs into a private R32_UINT target using the exact native
 * matrix and the private scene-depth copy, then schedules a 1x1 DO_NOT_WAIT readback.
 * @param context Selected renderer-owned immediate context.
 * @param batch Immutable pointer-free scene batch retained with the readback.
 * @param capturedFrame Captured-frame identity paired with the batch.
 * @param exactView Exact native matrix and viewport.
 * @param storage Sunrise-owned picking resources and readback ring.
 * @return Current picking readiness after scheduling or polling readback.
 */
[[nodiscard]] inspection::PickingReadiness
draw_pick_ids(ID3D11DeviceContext* context,
              const inspection::SceneFramePtr& batch,
              std::uint64_t capturedFrame,
              const inspection::RenderViewSnapshot& exactView,
              Storage& storage) noexcept;

/**
 * Releases every owned resource and clears descriptor state.
 * @param storage Sunrise-owned render resources to release and clear.
 */
void release(Storage& storage) noexcept;

/**
 * Reports whether a usable scene depth copy exists for occlusion testing.
 * @param storage Sunrise-owned render resources to inspect.
 * @return True when the depth copy and all required views are usable.
 */
[[nodiscard]] bool depth_available(const Storage& storage) noexcept;

} // namespace sunrise::client::hooks::graphics::renderer::debug_render
