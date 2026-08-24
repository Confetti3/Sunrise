#include "graphics_debug_render.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>
#include <iterator>

#include "../../../../core/logging/log.h"

namespace sunrise::client::hooks::graphics::renderer::debug_render {
void release_pick_targets(Storage& storage) noexcept;
void release_pick_pipeline(Storage& storage) noexcept;
namespace {

// ---------------------------------------------------------------------------
// Tuning constants. The scene depth comparison needs the game's projection near/far
// planes; these are research defaults, kept in one place for in-game calibration.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kFailureLimit = 3;
constexpr std::uint64_t kSlowOperationMicros = 4000;
/** One dynamic vertex buffer, sized for the inspector's worst-case line budget. */
constexpr UINT kMaximumVertices =
    static_cast<UINT>(::sunrise::client::inspection::kMaximumSceneLines) * 2U;
constexpr UINT kMaximumGlyphs = 1024;

constexpr char kShaderSource[] = R"(
cbuffer SceneConstants : register(b0) {
    row_major float4x4 viewProjection;
    float2 lineViewportSize;
    float lineHalfWidth;
    float linePadding;
};
struct VSInput {
    float3 position : POSITION;
    float4 color : COLOR;
};
struct VSOutput {
    float4 clipPosition : POSITION0;
    float4 color : COLOR;
};
struct LineGSOutput {
    float4 svPosition : SV_POSITION;
    float4 color : COLOR;
};
VSOutput vs_main(VSInput input) {
    VSOutput output;
    output.clipPosition = mul(viewProjection, float4(input.position, 1.0F));
    output.color = input.color;
    return output;
}
[maxvertexcount(4)]
void gs_main(line VSOutput input[2], inout TriangleStream<LineGSOutput> stream) {
    if (input[0].clipPosition.w <= 1.0e-5F
        || input[1].clipPosition.w <= 1.0e-5F) {
        return;
    }
    const float2 firstNdc = input[0].clipPosition.xy / input[0].clipPosition.w;
    const float2 secondNdc = input[1].clipPosition.xy / input[1].clipPosition.w;
    const float2 deltaPixels = (secondNdc - firstNdc) * 0.5F * lineViewportSize;
    const float lengthSquared = dot(deltaPixels, deltaPixels);
    if (lengthSquared < 1.0e-6F) {
        return;
    }
    const float2 normalPixels =
        float2(-deltaPixels.y, deltaPixels.x) * rsqrt(lengthSquared) * lineHalfWidth;
    const float2 normalNdc = normalPixels * 2.0F / lineViewportSize;
    LineGSOutput output;
    output.color = input[0].color;
    output.svPosition = input[0].clipPosition;
    output.svPosition.xy += normalNdc * input[0].clipPosition.w;
    stream.Append(output);
    output.svPosition = input[0].clipPosition;
    output.svPosition.xy -= normalNdc * input[0].clipPosition.w;
    stream.Append(output);
    output.color = input[1].color;
    output.svPosition = input[1].clipPosition;
    output.svPosition.xy += normalNdc * input[1].clipPosition.w;
    stream.Append(output);
    output.svPosition = input[1].clipPosition;
    output.svPosition.xy -= normalNdc * input[1].clipPosition.w;
    stream.Append(output);
}
struct GlyphVSInput {
    float3 position : POSITION;
    float4 color : COLOR;
    uint shape : SHAPE;
    float sizePixels : SIZE;
};
struct GlyphVSOutput {
    float4 clipPosition : POSITION0;
    float4 color : COLOR;
    nointerpolation uint shape : TEXCOORD0;
    float sizePixels : TEXCOORD1;
};
GlyphVSOutput vs_glyph(GlyphVSInput input) {
    GlyphVSOutput output;
    output.clipPosition = mul(viewProjection, float4(input.position, 1.0F));
    output.color = input.color;
    output.shape = input.shape;
    output.sizePixels = input.sizePixels;
    return output;
}
void glyph_vertex(float4 center,
                  float2 normalized,
                  float halfSize,
                  float4 color,
                  inout TriangleStream<LineGSOutput> stream) {
    LineGSOutput output;
    output.color = color;
    output.svPosition = center;
    output.svPosition.xy += normalized * halfSize * 2.0F / lineViewportSize * center.w;
    stream.Append(output);
}
void glyph_square(float4 center,
                  float halfSize,
                  float4 color,
                  inout TriangleStream<LineGSOutput> stream) {
    glyph_vertex(center, float2(-1,-1), halfSize, color, stream);
    glyph_vertex(center, float2(-1, 1), halfSize, color, stream);
    glyph_vertex(center, float2( 1,-1), halfSize, color, stream);
    glyph_vertex(center, float2( 1, 1), halfSize, color, stream);
    stream.RestartStrip();
}
static const float2 circlePoints[8] = {
    float2(1,0), float2(0.7071,0.7071), float2(0,1), float2(-0.7071,0.7071),
    float2(-1,0), float2(-0.7071,-0.7071), float2(0,-1), float2(0.7071,-0.7071)
};
void glyph_circle(float4 center,
                  float halfSize,
                  float4 color,
                  inout TriangleStream<LineGSOutput> stream) {
    [unroll]
    for (uint index = 0; index < 8; ++index) {
        glyph_vertex(center, float2(0,0), halfSize, color, stream);
        glyph_vertex(center, circlePoints[index], halfSize, color, stream);
        glyph_vertex(center, circlePoints[(index + 1) & 7], halfSize, color, stream);
        stream.RestartStrip();
    }
}
[maxvertexcount(64)]
void gs_glyph(point GlyphVSOutput input[1], inout TriangleStream<LineGSOutput> stream) {
    if (input[0].clipPosition.w <= 1.0e-5F) return;
    const float4 center = input[0].clipPosition;
    const float4 color = input[0].color;
    const float outerHalfSize = max((input[0].sizePixels + linePadding) * 0.5F, 1.0F);
    const float border = max(lineHalfWidth, 1.0F);
    const float innerHalfSize = max(outerHalfSize - border, 1.0F);
    const float luminance = dot(color.rgb, float3(0.2126F, 0.7152F, 0.0722F));
    const float3 borderRgb = luminance < 0.38F
                                 ? float3(243.0F / 255.0F,
                                          252.0F / 255.0F,
                                          240.0F / 255.0F)
                                 : float3(31.0F / 255.0F,
                                          39.0F / 255.0F,
                                          27.0F / 255.0F);
    const float4 borderColor = float4(borderRgb, saturate(color.a + 0.18F));
    if (input[0].shape == 0) {
        glyph_circle(center, outerHalfSize, borderColor, stream);
        glyph_circle(center, innerHalfSize, color, stream);
    } else {
        glyph_square(center, outerHalfSize, borderColor, stream);
        glyph_square(center, innerHalfSize, color, stream);
    }
}
float4 ps_hardware(LineGSOutput input) : SV_Target {
    return input.color;
}
float4 ps_selection_halo(LineGSOutput input) : SV_Target {
    return float4(243.0F / 255.0F, 252.0F / 255.0F, 240.0F / 255.0F, 1.0F);
}
float4 ps_hover(LineGSOutput input) : SV_Target {
    return float4(255.0F / 255.0F, 210.0F / 255.0F, 63.0F / 255.0F, 0.90F);
}
cbuffer PickConstants : register(b1) {
    row_major float4x4 pickViewProjection;
    float2 pickViewportSize;
    float pickHalfWidth;
    float pickPadding;
};
struct PickVSInput {
    float3 position : POSITION;
    uint token : PICKID;
};
struct PickVSOutput {
    float4 clipPosition : POSITION0;
    nointerpolation uint token : TEXCOORD0;
};
struct PickGSOutput {
    float4 svPosition : SV_POSITION;
    nointerpolation uint token : TEXCOORD0;
};
PickVSOutput vs_pick(PickVSInput input) {
    PickVSOutput output;
    output.clipPosition = mul(pickViewProjection, float4(input.position, 1.0F));
    output.token = input.token;
    return output;
}
struct PickGlyphVSInput {
    float3 position : POSITION;
    uint token : PICKID;
    uint shape : SHAPE;
    float sizePixels : SIZE;
};
struct PickGlyphVSOutput {
    float4 clipPosition : POSITION0;
    nointerpolation uint token : TEXCOORD0;
    nointerpolation uint shape : TEXCOORD1;
    float sizePixels : TEXCOORD2;
};
PickGlyphVSOutput vs_pick_glyph(PickGlyphVSInput input) {
    PickGlyphVSOutput output;
    output.clipPosition = mul(pickViewProjection, float4(input.position, 1.0F));
    output.token = input.token;
    output.shape = input.shape;
    output.sizePixels = input.sizePixels;
    return output;
}
void pick_glyph_vertex(float4 center,
                       float2 normalized,
                       float halfSize,
                       uint token,
                       inout TriangleStream<PickGSOutput> stream) {
    PickGSOutput output;
    output.token = token;
    output.svPosition = center;
    output.svPosition.xy += normalized * halfSize * 2.0F / pickViewportSize * center.w;
    stream.Append(output);
}
[maxvertexcount(24)]
void gs_pick_glyph(point PickGlyphVSOutput input[1],
                   inout TriangleStream<PickGSOutput> stream) {
    if (input[0].clipPosition.w <= 0.0F) return;
    const float halfSize = max(pickHalfWidth, input[0].sizePixels * 0.5F + 4.0F);
    if (input[0].shape == 0) {
        [unroll]
        for (uint index = 0; index < 8; ++index) {
            pick_glyph_vertex(input[0].clipPosition, float2(0,0), halfSize,
                              input[0].token, stream);
            pick_glyph_vertex(input[0].clipPosition, circlePoints[index], halfSize,
                              input[0].token, stream);
            pick_glyph_vertex(input[0].clipPosition, circlePoints[(index + 1) & 7], halfSize,
                              input[0].token, stream);
            stream.RestartStrip();
        }
    } else {
        pick_glyph_vertex(input[0].clipPosition, float2(-1,-1), halfSize,
                          input[0].token, stream);
        pick_glyph_vertex(input[0].clipPosition, float2(-1, 1), halfSize,
                          input[0].token, stream);
        pick_glyph_vertex(input[0].clipPosition, float2( 1,-1), halfSize,
                          input[0].token, stream);
        pick_glyph_vertex(input[0].clipPosition, float2( 1, 1), halfSize,
                          input[0].token, stream);
        stream.RestartStrip();
    }
}
[maxvertexcount(4)]
void gs_pick(line PickVSOutput input[2], inout TriangleStream<PickGSOutput> stream) {
    if (input[0].clipPosition.w <= 0.0F || input[1].clipPosition.w <= 0.0F) {
        return;
    }
    const float2 firstNdc = input[0].clipPosition.xy / input[0].clipPosition.w;
    const float2 secondNdc = input[1].clipPosition.xy / input[1].clipPosition.w;
    const float2 deltaPixels = (secondNdc - firstNdc) * 0.5F * pickViewportSize;
    const float lengthSquared = dot(deltaPixels, deltaPixels);
    if (lengthSquared < 1.0e-6F) {
        return;
    }
    const float2 normalPixels =
        float2(-deltaPixels.y, deltaPixels.x) * rsqrt(lengthSquared) * pickHalfWidth;
    const float2 normalNdc = normalPixels * 2.0F / pickViewportSize;
    PickGSOutput output;
    output.token = input[0].token;
    output.svPosition = input[0].clipPosition;
    output.svPosition.xy += normalNdc * input[0].clipPosition.w;
    stream.Append(output);
    output.svPosition = input[0].clipPosition;
    output.svPosition.xy -= normalNdc * input[0].clipPosition.w;
    stream.Append(output);
    output.svPosition = input[1].clipPosition;
    output.svPosition.xy += normalNdc * input[1].clipPosition.w;
    stream.Append(output);
    output.svPosition = input[1].clipPosition;
    output.svPosition.xy -= normalNdc * input[1].clipPosition.w;
    stream.Append(output);
}
uint ps_pick(PickGSOutput input) : SV_Target {
    return input.token;
}
Texture2DMS<uint> pickIds : register(t1);
RWTexture2D<uint> resolvedPick : register(u0);
cbuffer PickResolveConstants : register(b2) {
    uint2 pickPixel;
    uint pickSampleCount;
    uint pickResolvePadding;
};
[numthreads(1, 1, 1)]
void cs_pick_resolve(uint3 dispatchThread : SV_DispatchThreadID) {
    uint token = 0;
    [unroll]
    for (uint index = 0; index < 8; ++index) {
        if (index < pickSampleCount) {
            const uint candidate = pickIds.Load(int2(pickPixel), index);
            if (token == 0 && candidate != 0) {
                token = candidate;
            }
        }
    }
    resolvedPick[uint2(0, 0)] = token;
}
)";

struct PickVertex final {
    std::array<float, 3> position{};
    std::uint32_t token{};
};

struct GlyphVertex final {
    std::array<float, 3> position{};
    std::array<float, 4> color{};
    std::uint32_t shape{};
    float sizePixels{};
};

struct PickGlyphVertex final {
    std::array<float, 3> position{};
    std::uint32_t token{};
    std::uint32_t shape{};
    float sizePixels{};
};

struct PickConstants final {
    std::array<float, 16> viewProjection{};
    std::array<float, 2> viewportSize{};
    float halfWidth{3.0F};
    float padding{};
};

struct PickResolveConstants final {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t samples{};
    std::uint32_t padding{};
};

struct Compiler final {
    HMODULE library{};
    pD3DCompile compile{};
};

/** @return A process-wide compiler handle, or an empty one when unavailable. */
[[nodiscard]] Compiler compiler() noexcept {
    static const Compiler cached = []() noexcept {
        Compiler loaded{};
        // Loaded dynamically so the build keeps no hard d3dcompiler link dependency.
        loaded.library = LoadLibraryW(L"d3dcompiler_47.dll");
        if (loaded.library == nullptr) {
            return loaded;
        }
        loaded.compile = reinterpret_cast<pD3DCompile>(
            reinterpret_cast<void*>(GetProcAddress(loaded.library, "D3DCompile")));
        if (loaded.compile == nullptr) {
            FreeLibrary(loaded.library);
            loaded.library = nullptr;
        }
        return loaded;
    }();
    return cached;
}

template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

void release_objects(Storage& storage) noexcept {
    release_com(storage.depthStencilView);
    release_com(storage.depthView);
    release_com(storage.depthTexture);
}

void note(const char* event, const char* reason) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=renderer stage=debug_render %s reason=%s", event, reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Maps a game depth-stencil descriptor onto a typeless-family storage format plus the
 * shader-resource view format that reads depth from lane x.
 */
[[nodiscard]] bool depth_formats(DXGI_FORMAT source,
                                 DXGI_FORMAT& storageFormat,
                                 DXGI_FORMAT& viewFormat,
                                 DXGI_FORMAT& depthStencilFormat) noexcept {
    switch (source) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        storageFormat = DXGI_FORMAT_R32_TYPELESS;
        viewFormat = DXGI_FORMAT_R32_FLOAT;
        depthStencilFormat = DXGI_FORMAT_D32_FLOAT;
        return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        storageFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
        viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        depthStencilFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        return true;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        storageFormat = DXGI_FORMAT_R24G8_TYPELESS;
        viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        storageFormat = DXGI_FORMAT_R16_TYPELESS;
        viewFormat = DXGI_FORMAT_R16_UNORM;
        depthStencilFormat = DXGI_FORMAT_D16_UNORM;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool descriptor_matches(const Storage& storage,
                                      const D3D11_TEXTURE2D_DESC& source) noexcept {
    return storage.width == source.Width && storage.height == source.Height
           && storage.sourceFormat == source.Format
           && storage.sourceSamples == source.SampleDesc.Count
           && storage.sourceQuality == source.SampleDesc.Quality;
}

void capture_depth(ID3D11Device* device,
                   ID3D11DeviceContext* context,
                   ID3D11DepthStencilView* depthStencil,
                   bool reversedZ,
                   std::uint64_t sourceSequence,
                   Storage& storage) noexcept {
    ID3D11Resource* resource = nullptr;
    depthStencil->GetResource(&resource);
    if (resource == nullptr) {
        return;
    }
    ID3D11Texture2D* source = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void**>(&source)))) {
        release_com(resource);
        return;
    }
    release_com(resource);

    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    DXGI_FORMAT storageFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT viewFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT depthStencilFormat{DXGI_FORMAT_UNKNOWN};
    storage.capability = evaluate_depth_descriptor(
        description.Width, description.Height, description.SampleDesc.Count, description.Format);
    if (storage.capability != inspection::DepthCapability::supported) {
        if (!storage.rejected) {
            storage.rejected = true;
            note("result=reject", inspection::depth_capability_name(storage.capability));
        }
        release_com(source);
        return;
    }
    if (!depth_formats(description.Format, storageFormat, viewFormat, depthStencilFormat)) {
        if (!storage.rejected) {
            storage.rejected = true;
            note("result=reject", "unsupported_depth_format");
        }
        release_com(source);
        return;
    }
    if (!descriptor_matches(storage, description)) {
        release_objects(storage);
        release_pick_targets(storage);
        storage.width = description.Width;
        storage.height = description.Height;
        storage.sourceFormat = description.Format;
        storage.sourceSamples = description.SampleDesc.Count;
        storage.sourceQuality = description.SampleDesc.Quality;
        storage.consecutiveFailures = 0;
    }
    const D3D11_COMPARISON_FUNC comparison =
        reversedZ ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
    if (storage.depthComparison != comparison) {
        storage.depthComparison = comparison;
        release_pick_pipeline(storage);
    }

    if (storage.depthTexture == nullptr || storage.depthView == nullptr
        || storage.depthStencilView == nullptr) {
        D3D11_TEXTURE2D_DESC target = description;
        target.Format = storageFormat;
        target.MipLevels = 1;
        target.ArraySize = 1;
        target.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
        target.MiscFlags = 0;

        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = viewFormat;
        view.ViewDimension = description.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS
                                                              : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (description.SampleDesc.Count == 1) {
            view.Texture2D.MostDetailedMip = 0;
            view.Texture2D.MipLevels = 1;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};
        depthView.Format = depthStencilFormat;
        depthView.ViewDimension = description.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                                                   : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (description.SampleDesc.Count == 1) {
            depthView.Texture2D.MipSlice = 0;
        }

        if (FAILED(device->CreateTexture2D(&target, nullptr, &storage.depthTexture))
            || FAILED(
                device->CreateShaderResourceView(storage.depthTexture, &view, &storage.depthView))
            || FAILED(device->CreateDepthStencilView(
                storage.depthTexture, &depthView, &storage.depthStencilView))) {
            release_objects(storage);
            storage.rejected = true;
            storage.capability = inspection::DepthCapability::captureFailed;
            note("result=fail", "create_depth_copy");
            release_com(source);
            return;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         "ev=renderer stage=debug_render result=depth_captured");
    }

    // Same dimensions, same sample count, same typeless family: a straight copy keeps
    // every multisample so the pixel shader can take the closest one.
    context->CopyResource(storage.depthTexture, source);
    storage.captureSequence = sourceSequence != 0 ? sourceSequence : storage.captureSequence + 1U;
    release_com(source);
}

/** Compiles the embedded line shader pair once per device session. */
[[nodiscard]] bool create_pipeline(ID3D11Device* device, Storage& storage) noexcept {
    const Compiler tools = compiler();
    if (tools.compile == nullptr) {
        note("result=fail", "d3dcompiler_unavailable");
        return false;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    auto compile_blob = [&](const char* entry, const char* targetVersion, ID3DBlob** blob) {
        return SUCCEEDED(tools.compile(kShaderSource,
                                       std::strlen(kShaderSource),
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       entry,
                                       targetVersion,
                                       0,
                                       0,
                                       blob,
                                       &errors));
    };
    if (!compile_blob("vs_main", "vs_5_0", &code)) {
        note("result=fail", "compile_vertex_shader");
        release_com(errors);
        return false;
    }
    const HRESULT vertexResult = device->CreateVertexShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.vertexShader);
    if (FAILED(vertexResult)) {
        release_com(code);
        release_com(errors);
        note("result=fail", "create_vertex_shader");
        return false;
    }

    constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    const HRESULT layoutResult = device->CreateInputLayout(elements,
                                                           static_cast<UINT>(std::size(elements)),
                                                           code->GetBufferPointer(),
                                                           code->GetBufferSize(),
                                                           &storage.inputLayout);
    release_com(code);
    if (FAILED(layoutResult)) {
        note("result=fail", "create_input_layout");
        return false;
    }

    code = nullptr;
    if (!compile_blob("vs_glyph", "vs_5_0", &code)) {
        note("result=fail", "compile_glyph_vertex_shader");
        release_com(errors);
        return false;
    }
    constexpr D3D11_INPUT_ELEMENT_DESC glyphElements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"SHAPE", 0, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"SIZE", 0, DXGI_FORMAT_R32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (FAILED(device->CreateVertexShader(
            code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.glyphVertexShader))
        || FAILED(device->CreateInputLayout(glyphElements,
                                            static_cast<UINT>(std::size(glyphElements)),
                                            code->GetBufferPointer(),
                                            code->GetBufferSize(),
                                            &storage.glyphInputLayout))) {
        release_com(code);
        release_com(errors);
        note("result=fail", "create_glyph_vertex_pipeline");
        return false;
    }
    release_com(code);

    code = nullptr;
    if (!compile_blob("gs_glyph", "gs_5_0", &code)
        || FAILED(device->CreateGeometryShader(code->GetBufferPointer(),
                                               code->GetBufferSize(),
                                               nullptr,
                                               &storage.glyphGeometryShader))) {
        release_com(code);
        release_com(errors);
        note("result=fail", "create_glyph_geometry_shader");
        return false;
    }
    release_com(code);

    code = nullptr;
    if (!compile_blob("gs_main", "gs_5_0", &code)) {
        note("result=fail", "compile_line_geometry_shader");
        release_com(errors);
        return false;
    }
    const HRESULT geometryResult = device->CreateGeometryShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.lineGeometryShader);
    release_com(code);
    release_com(errors);
    if (FAILED(geometryResult)) {
        note("result=fail", "create_line_geometry_shader");
        return false;
    }

    code = nullptr;
    if (!compile_blob("ps_hardware", "ps_5_0", &code)) {
        note("result=fail", "compile_pixel_shader");
        release_com(errors);
        return false;
    }
    const HRESULT pixelResult = device->CreatePixelShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.pixelShader);
    release_com(code);
    release_com(errors);
    if (FAILED(pixelResult)) {
        note("result=fail", "create_pixel_shader");
        return false;
    }
    code = nullptr;
    if (!compile_blob("ps_selection_halo", "ps_5_0", &code)) {
        note("result=fail", "compile_selection_halo_pixel_shader");
        release_com(errors);
        return false;
    }
    const HRESULT selectionHaloResult =
        device->CreatePixelShader(code->GetBufferPointer(),
                                  code->GetBufferSize(),
                                  nullptr,
                                  &storage.selectionHaloPixelShader);
    release_com(code);
    release_com(errors);
    if (FAILED(selectionHaloResult)) {
        note("result=fail", "create_selection_halo_pixel_shader");
        return false;
    }
    code = nullptr;
    if (!compile_blob("ps_hover", "ps_5_0", &code)) {
        note("result=fail", "compile_hover_pixel_shader");
        release_com(errors);
        return false;
    }
    const HRESULT hoverResult = device->CreatePixelShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.hoverPixelShader);
    release_com(code);
    release_com(errors);
    if (FAILED(hoverResult)) {
        note("result=fail", "create_hover_pixel_shader");
        return false;
    }

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(std::array<float, 20>);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&constants, nullptr, &storage.constantBuffer))) {
        note("result=fail", "create_constant_buffer");
        return false;
    }

    D3D11_BUFFER_DESC vertices{};
    vertices.ByteWidth = kMaximumVertices * static_cast<UINT>(sizeof(inspection::SceneVertex));
    vertices.Usage = D3D11_USAGE_DYNAMIC;
    vertices.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertices.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&vertices, nullptr, &storage.vertexBuffer))) {
        note("result=fail", "create_vertex_buffer");
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, &storage.blendState))) {
        note("result=fail", "create_blend_state");
        return false;
    }
    vertices.ByteWidth = kMaximumGlyphs * static_cast<UINT>(sizeof(GlyphVertex));
    if (FAILED(device->CreateBuffer(&vertices, nullptr, &storage.glyphVertexBuffer))) {
        note("result=fail", "create_glyph_vertex_buffer");
        return false;
    }
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer, &storage.lineRasterizerState))) {
        note("result=fail", "create_line_rasterizer");
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depth, &storage.standardDepthState))) {
        return false;
    }
    depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depth, &storage.reversedDepthState))) {
        return false;
    }
    depth.DepthEnable = FALSE;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateDepthStencilState(&depth, &storage.selectionHaloDepthState))) {
        return false;
    }
    return true;
}

} // namespace

void release_pick_targets(Storage& storage) noexcept {
    release_com(storage.pickTarget);
    release_com(storage.pickView);
    release_com(storage.pickTexture);
    release_com(storage.pickDepthStencilView);
    release_com(storage.pickDepthTexture);
    release_com(storage.pickResolvedView);
    release_com(storage.pickResolved);
}

void release_pick_pipeline(Storage& storage) noexcept {
    release_com(storage.pickVertexBuffer);
    release_com(storage.pickGlyphVertexBuffer);
    release_com(storage.pickConstantBuffer);
    release_com(storage.pickResolveConstantBuffer);
    release_com(storage.pickVertexShader);
    release_com(storage.pickGeometryShader);
    release_com(storage.pickGlyphVertexShader);
    release_com(storage.pickGlyphGeometryShader);
    release_com(storage.pickPixelShader);
    release_com(storage.pickResolveShader);
    release_com(storage.pickInputLayout);
    release_com(storage.pickGlyphInputLayout);
    release_com(storage.pickDepthState);
    release_com(storage.pickRasterizerState);
}

[[nodiscard]] bool load_pick_request(inspection::PickRequest& request) noexcept {
    request = inspection::pick_request();
    return request.sequence != 0;
}

[[nodiscard]] bool create_pick_pipeline(ID3D11Device* device, Storage& storage) noexcept {
    const Compiler tools = compiler();
    if (device == nullptr || tools.compile == nullptr) {
        return false;
    }
    const auto compile_shader =
        [&](const char* entry, const char* target, ID3DBlob** output) noexcept {
            ID3DBlob* errors = nullptr;
            const HRESULT result = tools.compile(kShaderSource,
                                                 std::strlen(kShaderSource),
                                                 nullptr,
                                                 nullptr,
                                                 nullptr,
                                                 entry,
                                                 target,
                                                 0,
                                                 0,
                                                 output,
                                                 &errors);
            release_com(errors);
            return SUCCEEDED(result);
        };

    ID3DBlob* code = nullptr;
    if (!compile_shader("vs_pick", "vs_5_0", &code)
        || FAILED(device->CreateVertexShader(
            code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.pickVertexShader))) {
        release_com(code);
        return false;
    }
    constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PICKID", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (FAILED(device->CreateInputLayout(elements,
                                         static_cast<UINT>(std::size(elements)),
                                         code->GetBufferPointer(),
                                         code->GetBufferSize(),
                                         &storage.pickInputLayout))) {
        release_com(code);
        return false;
    }
    release_com(code);

    if (!compile_shader("vs_pick_glyph", "vs_5_0", &code)
        || FAILED(device->CreateVertexShader(code->GetBufferPointer(),
                                             code->GetBufferSize(),
                                             nullptr,
                                             &storage.pickGlyphVertexShader))) {
        release_com(code);
        return false;
    }
    constexpr D3D11_INPUT_ELEMENT_DESC glyphElements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PICKID", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"SHAPE", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"SIZE", 0, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    if (FAILED(device->CreateInputLayout(glyphElements,
                                         static_cast<UINT>(std::size(glyphElements)),
                                         code->GetBufferPointer(),
                                         code->GetBufferSize(),
                                         &storage.pickGlyphInputLayout))) {
        release_com(code);
        return false;
    }
    release_com(code);
    if (!compile_shader("gs_pick_glyph", "gs_5_0", &code)
        || FAILED(device->CreateGeometryShader(code->GetBufferPointer(),
                                               code->GetBufferSize(),
                                               nullptr,
                                               &storage.pickGlyphGeometryShader))) {
        release_com(code);
        return false;
    }
    release_com(code);

    if (!compile_shader("gs_pick", "gs_5_0", &code)
        || FAILED(device->CreateGeometryShader(code->GetBufferPointer(),
                                               code->GetBufferSize(),
                                               nullptr,
                                               &storage.pickGeometryShader))) {
        release_com(code);
        return false;
    }
    release_com(code);
    if (!compile_shader("ps_pick", "ps_5_0", &code)
        || FAILED(device->CreatePixelShader(
            code->GetBufferPointer(), code->GetBufferSize(), nullptr, &storage.pickPixelShader))) {
        release_com(code);
        return false;
    }
    release_com(code);
    if (!compile_shader("cs_pick_resolve", "cs_5_0", &code)
        || FAILED(device->CreateComputeShader(code->GetBufferPointer(),
                                              code->GetBufferSize(),
                                              nullptr,
                                              &storage.pickResolveShader))) {
        release_com(code);
        return false;
    }
    release_com(code);

    D3D11_BUFFER_DESC vertices{};
    vertices.ByteWidth = kMaximumVertices * static_cast<UINT>(sizeof(PickVertex));
    vertices.Usage = D3D11_USAGE_DYNAMIC;
    vertices.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertices.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&vertices, nullptr, &storage.pickVertexBuffer))) {
        return false;
    }
    vertices.ByteWidth = kMaximumGlyphs * static_cast<UINT>(sizeof(PickGlyphVertex));
    if (FAILED(device->CreateBuffer(&vertices, nullptr, &storage.pickGlyphVertexBuffer))) {
        return false;
    }
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(PickConstants);
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&constants, nullptr, &storage.pickConstantBuffer))) {
        return false;
    }
    constants.ByteWidth = sizeof(PickResolveConstants);
    if (FAILED(device->CreateBuffer(&constants, nullptr, &storage.pickResolveConstantBuffer))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    // Picking owns a separate copy of scene depth, so helper depths may be written
    // to resolve overlapping IDs to the nearest visible segment.
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = storage.depthComparison;
    if (FAILED(device->CreateDepthStencilState(&depth, &storage.pickDepthState))) {
        return false;
    }
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer, &storage.pickRasterizerState))) {
        return false;
    }
    return true;
}

[[nodiscard]] bool create_pick_targets(ID3D11Device* device, Storage& storage) noexcept {
    bool readbacksReady = true;
    for (const Storage::PickReadback& readback : storage.pickReadbacks) {
        readbacksReady = readbacksReady && readback.staging != nullptr;
    }
    const bool resolveReady = storage.sourceSamples == 1
                              || (storage.pickView != nullptr && storage.pickResolved != nullptr
                                  && storage.pickResolvedView != nullptr);
    if (storage.pickTexture != nullptr && storage.pickTarget != nullptr
        && storage.pickDepthTexture != nullptr && storage.pickDepthStencilView != nullptr
        && resolveReady && readbacksReady) {
        return true;
    }
    release_pick_targets(storage);
    if (device == nullptr || storage.width == 0 || storage.height == 0
        || storage.sourceSamples == 0) {
        return false;
    }
    UINT qualityLevels = 1;
    if (storage.sourceSamples > 1
        && (FAILED(device->CheckMultisampleQualityLevels(
                DXGI_FORMAT_R32_UINT, storage.sourceSamples, &qualityLevels))
            || qualityLevels == 0 || storage.sourceQuality >= qualityLevels)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC ids{};
    ids.Width = storage.width;
    ids.Height = storage.height;
    ids.MipLevels = 1;
    ids.ArraySize = 1;
    ids.Format = DXGI_FORMAT_R32_UINT;
    ids.SampleDesc.Count = storage.sourceSamples;
    ids.SampleDesc.Quality = storage.sourceQuality;
    ids.Usage = D3D11_USAGE_DEFAULT;
    ids.BindFlags =
        D3D11_BIND_RENDER_TARGET | (storage.sourceSamples > 1 ? D3D11_BIND_SHADER_RESOURCE : 0U);
    if (FAILED(device->CreateTexture2D(&ids, nullptr, &storage.pickTexture))) {
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC target{};
    target.Format = DXGI_FORMAT_R32_UINT;
    target.ViewDimension =
        storage.sourceSamples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    if (storage.sourceSamples == 1) {
        target.Texture2D.MipSlice = 0;
    }
    if (FAILED(device->CreateRenderTargetView(storage.pickTexture, &target, &storage.pickTarget))) {
        release_pick_targets(storage);
        return false;
    }
    if (storage.depthTexture == nullptr) {
        release_pick_targets(storage);
        return false;
    }
    D3D11_TEXTURE2D_DESC pickDepth{};
    storage.depthTexture->GetDesc(&pickDepth);
    pickDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    pickDepth.MiscFlags = 0;
    DXGI_FORMAT ignoredStorageFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT ignoredViewFormat{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT pickDepthViewFormat{DXGI_FORMAT_UNKNOWN};
    if (!depth_formats(
            storage.sourceFormat, ignoredStorageFormat, ignoredViewFormat, pickDepthViewFormat)
        || FAILED(device->CreateTexture2D(&pickDepth, nullptr, &storage.pickDepthTexture))) {
        release_pick_targets(storage);
        return false;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC pickDepthView{};
    pickDepthView.Format = pickDepthViewFormat;
    pickDepthView.ViewDimension =
        storage.sourceSamples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (storage.sourceSamples == 1) {
        pickDepthView.Texture2D.MipSlice = 0;
    }
    if (FAILED(device->CreateDepthStencilView(
            storage.pickDepthTexture, &pickDepthView, &storage.pickDepthStencilView))) {
        release_pick_targets(storage);
        return false;
    }
    D3D11_TEXTURE2D_DESC resolved{};
    resolved.Width = 1;
    resolved.Height = 1;
    resolved.MipLevels = 1;
    resolved.ArraySize = 1;
    resolved.Format = DXGI_FORMAT_R32_UINT;
    resolved.SampleDesc.Count = 1;
    if (storage.sourceSamples > 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_UINT;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        if (FAILED(
                device->CreateShaderResourceView(storage.pickTexture, &view, &storage.pickView))) {
            release_pick_targets(storage);
            return false;
        }
        resolved.Usage = D3D11_USAGE_DEFAULT;
        resolved.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&resolved, nullptr, &storage.pickResolved))
            || FAILED(device->CreateUnorderedAccessView(
                storage.pickResolved, nullptr, &storage.pickResolvedView))) {
            release_pick_targets(storage);
            return false;
        }
    }
    resolved.Usage = D3D11_USAGE_STAGING;
    resolved.BindFlags = 0;
    resolved.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (Storage::PickReadback& readback : storage.pickReadbacks) {
        if (readback.staging == nullptr
            && FAILED(device->CreateTexture2D(&resolved, nullptr, &readback.staging))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inspection::PickingReadiness poll_pick_readbacks(ID3D11DeviceContext* context,
                                                               Storage& storage) noexcept {
    for (Storage::PickReadback& readback : storage.pickReadbacks) {
        if (!readback.pending || readback.staging == nullptr) {
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result =
            context->Map(readback.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            continue;
        }
        if (FAILED(result)) {
            readback.pending = false;
            readback.batch.reset();
            continue;
        }
        const std::uint32_t token = *static_cast<const std::uint32_t*>(mapped.pData);
        context->Unmap(readback.staging, 0);
        inspection::PickResult completed{};
        completed.requestSequence = readback.requestSequence;
        completed.capturedFrame = readback.capturedFrame;
        completed.engineFrame = readback.engineFrame;
        completed.viewPublication = readback.viewPublication;
        completed.depthSequence = readback.depthSequence;
        completed.graphGeneration = readback.graphGeneration;
        if (readback.batch) {
            completed.node = inspection::resolve_pick_token(*readback.batch, token);
        }
        completed.ready = true;
        if (completed.requestSequence >= inspection::pick_result().requestSequence) {
            inspection::publish_pick_result(completed);
        }
        readback.pending = false;
        readback.batch.reset();
    }
    return inspection::pick_result().ready ? inspection::PickingReadiness::ready
                                           : inspection::PickingReadiness::waitingForReadback;
}

struct PickPipelineState final {
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
    ID3D11DepthStencilView* depth{};
    ID3D11DepthStencilState* depthState{};
    UINT stencilReference{};
    ID3D11BlendState* blend{};
    FLOAT blendFactors[4]{};
    UINT sampleMask{};
    ID3D11RasterizerState* rasterizer{};
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        viewports{};
    UINT viewportCount{static_cast<UINT>(viewports.size())};
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    UINT scissorCount{static_cast<UINT>(scissors.size())};
    ID3D11InputLayout* layout{};
    ID3D11Buffer* vertexBuffer{};
    UINT vertexStride{};
    UINT vertexOffset{};
    ID3D11Buffer* indexBuffer{};
    DXGI_FORMAT indexFormat{DXGI_FORMAT_UNKNOWN};
    UINT indexOffset{};
    D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
    ID3D11VertexShader* vertexShader{};
    ID3D11Buffer* vertexConstants{};
    ID3D11GeometryShader* geometryShader{};
    ID3D11Buffer* geometryConstants{};
    ID3D11HullShader* hullShader{};
    ID3D11DomainShader* domainShader{};
    ID3D11PixelShader* pixelShader{};
    std::array<ID3D11ShaderResourceView*, 2> pixelResources{};
    ID3D11ComputeShader* computeShader{};
    ID3D11Buffer* computeConstants{};
    ID3D11ShaderResourceView* computeResource{};
    ID3D11UnorderedAccessView* computeTarget{};

    void capture(ID3D11DeviceContext* context) noexcept {
        context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), &depth);
        context->OMGetDepthStencilState(&depthState, &stencilReference);
        context->OMGetBlendState(&blend, blendFactors, &sampleMask);
        context->RSGetState(&rasterizer);
        context->RSGetViewports(&viewportCount, viewports.data());
        context->RSGetScissorRects(&scissorCount, scissors.data());
        context->IAGetInputLayout(&layout);
        context->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        context->IAGetPrimitiveTopology(&topology);
        context->VSGetShader(&vertexShader, nullptr, nullptr);
        context->VSGetConstantBuffers(1, 1, &vertexConstants);
        context->GSGetShader(&geometryShader, nullptr, nullptr);
        context->GSGetConstantBuffers(1, 1, &geometryConstants);
        context->HSGetShader(&hullShader, nullptr, nullptr);
        context->DSGetShader(&domainShader, nullptr, nullptr);
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        context->PSGetShaderResources(
            0, static_cast<UINT>(pixelResources.size()), pixelResources.data());
        context->CSGetShader(&computeShader, nullptr, nullptr);
        context->CSGetConstantBuffers(2, 1, &computeConstants);
        context->CSGetShaderResources(1, 1, &computeResource);
        context->CSGetUnorderedAccessViews(0, 1, &computeTarget);
    }

    void restore(ID3D11DeviceContext* context) noexcept {
        context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth);
        context->OMSetDepthStencilState(depthState, stencilReference);
        context->OMSetBlendState(blend, blendFactors, sampleMask);
        context->RSSetState(rasterizer);
        context->RSSetViewports(viewportCount, viewports.data());
        context->RSSetScissorRects(scissorCount, scissors.data());
        context->IASetInputLayout(layout);
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        context->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vertexShader, nullptr, 0);
        context->VSSetConstantBuffers(1, 1, &vertexConstants);
        context->GSSetShader(geometryShader, nullptr, 0);
        context->GSSetConstantBuffers(1, 1, &geometryConstants);
        context->HSSetShader(hullShader, nullptr, 0);
        context->DSSetShader(domainShader, nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->PSSetShaderResources(
            0, static_cast<UINT>(pixelResources.size()), pixelResources.data());
        context->CSSetShader(computeShader, nullptr, 0);
        context->CSSetConstantBuffers(2, 1, &computeConstants);
        context->CSSetShaderResources(1, 1, &computeResource);
        constexpr UINT keepCounter = D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
        context->CSSetUnorderedAccessViews(0, 1, &computeTarget, &keepCounter);
    }

    void release() noexcept {
        for (ID3D11RenderTargetView*& target : targets) {
            release_com(target);
        }
        release_com(depth);
        release_com(depthState);
        release_com(blend);
        release_com(rasterizer);
        release_com(layout);
        release_com(vertexBuffer);
        release_com(indexBuffer);
        release_com(vertexShader);
        release_com(vertexConstants);
        release_com(geometryShader);
        release_com(geometryConstants);
        release_com(hullShader);
        release_com(domainShader);
        release_com(pixelShader);
        for (ID3D11ShaderResourceView*& resource : pixelResources) {
            release_com(resource);
        }
        release_com(computeShader);
        release_com(computeConstants);
        release_com(computeResource);
        release_com(computeTarget);
    }
};

void update_depth(ID3D11Device* device, ID3D11DeviceContext* context, Storage& storage) noexcept {
    if (device == nullptr || context == nullptr || storage.rejected) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    ID3D11DepthStencilView* depthStencil = nullptr;
    context->OMGetRenderTargets(0, nullptr, &depthStencil);
    if (depthStencil != nullptr) {
        capture_depth(device, context, depthStencil, false, storage.captureSequence + 1U, storage);
        depthStencil->Release();
    } else if (++storage.consecutiveFailures >= kFailureLimit) {
        storage.rejected = true;
        storage.capability = inspection::DepthCapability::captureFailed;
        note("result=circuit_open", "depth_stencil_unavailable");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    storage.lastCaptureMicros = static_cast<std::uint64_t>(elapsed.count());
    storage.maximumCaptureMicros =
        (std::max)(storage.maximumCaptureMicros, storage.lastCaptureMicros);
    if (storage.lastCaptureMicros >= kSlowOperationMicros && !storage.slowCaptureReported) {
        storage.slowCaptureReported = true;
        note("result=slow", "capture_over_budget");
    }
}

void update_depth_from_view(ID3D11Device* device,
                            ID3D11DeviceContext* context,
                            ID3D11DepthStencilView* source,
                            bool reversedZ,
                            std::uint64_t sourceSequence,
                            Storage& storage) noexcept {
    if (device == nullptr || context == nullptr || source == nullptr || sourceSequence == 0
        || storage.rejected) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    capture_depth(device, context, source, reversedZ, sourceSequence, storage);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    storage.lastCaptureMicros = static_cast<std::uint64_t>(elapsed.count());
    storage.maximumCaptureMicros =
        (std::max)(storage.maximumCaptureMicros, storage.lastCaptureMicros);
    if (storage.lastCaptureMicros >= kSlowOperationMicros && !storage.slowCaptureReported) {
        storage.slowCaptureReported = true;
        note("result=slow", "capture_over_budget");
    }
}

bool draw_lines(ID3D11DeviceContext* context,
                ID3D11RenderTargetView* target,
                const SceneLine* lines,
                std::size_t lineCount,
                Storage& storage,
                const inspection::RenderViewSnapshot& exactView,
                const SceneGlyph* glyphs,
                std::size_t glyphCount,
                float lineWidthPixels) noexcept {
    const auto started = std::chrono::steady_clock::now();
    if (context == nullptr || target == nullptr || (lineCount == 0 && glyphCount == 0)
        || (lineCount != 0 && lines == nullptr) || (glyphCount != 0 && glyphs == nullptr)
        || storage.depthStencilView == nullptr || !exactView.valid || !exactView.exactNative
        || exactView.engineFrame == 0 || exactView.publication == 0
        || exactView.viewport.width <= 0.0F || exactView.viewport.height <= 0.0F) {
        return false;
    }
    if (storage.vertexShader == nullptr || storage.glyphVertexShader == nullptr) {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        const bool created = device != nullptr && create_pipeline(device, storage);
        if (device != nullptr) {
            device->Release();
        }
        if (!created) {
            storage.rejected = true;
            storage.capability = inspection::DepthCapability::captureFailed;
            return false;
        }
    }
    if (lineCount > inspection::kMaximumSceneLines) {
        lineCount = inspection::kMaximumSceneLines;
    }
    glyphCount = (std::min)(glyphCount, static_cast<std::size_t>(kMaximumGlyphs));
    const float baseHalfWidth = (std::clamp)(lineWidthPixels, 1.0F, 4.0F) * 0.5F;

    // Upload this frame's lines before any pipeline state is borrowed, so an upload
    // failure cannot strand references.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (lineCount != 0) {
        if (FAILED(context->Map(storage.vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            note("result=fail", "map_vertex_buffer");
            return false;
        }
        auto* vertices = static_cast<inspection::SceneVertex*>(mapped.pData);
        for (std::size_t index = 0; index < lineCount; ++index) {
            vertices[index * 2U] = lines[index].first;
            vertices[index * 2U + 1U] = lines[index].second;
        }
        context->Unmap(storage.vertexBuffer, 0);
    }
    if (glyphCount != 0) {
        if (FAILED(
                context->Map(storage.glyphVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            note("result=fail", "map_glyph_vertex_buffer");
            return false;
        }
        auto* glyphVertices = static_cast<GlyphVertex*>(mapped.pData);
        for (std::size_t index = 0; index < glyphCount; ++index) {
            glyphVertices[index] = {glyphs[index].position,
                                    glyphs[index].color,
                                    static_cast<std::uint32_t>(glyphs[index].shape),
                                    glyphs[index].sizePixels};
        }
        context->Unmap(storage.glyphVertexBuffer, 0);
    }

    // Camera constants matching the inspector projection convention.
    D3D11_MAPPED_SUBRESOURCE constantsMapped{};
    if (FAILED(context->Map(
            storage.constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &constantsMapped))) {
        note("result=fail", "map_constant_buffer");
        return false;
    }
    auto* constants = static_cast<std::array<float, 20>*>(constantsMapped.pData);
    const auto& matrix = exactView.viewProjection;
    const float lineViewportWidth = exactView.viewport.width;
    const float lineViewportHeight = exactView.viewport.height;
    std::memcpy(constants->data(), matrix.data(), sizeof(matrix));
    (*constants)[16U] = lineViewportWidth;
    (*constants)[17U] = lineViewportHeight;
    (*constants)[18U] = baseHalfWidth;
    (*constants)[19U] = 0.0F;
    context->Unmap(storage.constantBuffer, 0);

    // Target size drives the viewport; the game's own viewport is restored below.
    ID3D11Resource* targetResource = nullptr;
    target->GetResource(&targetResource);
    D3D11_TEXTURE2D_DESC targetDescription{};
    if (targetResource != nullptr) {
        if (ID3D11Texture2D* targetTexture = nullptr;
            SUCCEEDED(targetResource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                     reinterpret_cast<void**>(&targetTexture)))
            && targetTexture != nullptr) {
            targetTexture->GetDesc(&targetDescription);
            targetTexture->Release();
        }
        targetResource->Release();
    }
    if (targetDescription.Width == 0 || targetDescription.Height == 0) {
        note("result=fail", "invalid_target_dimensions");
        return false;
    }
    if (targetDescription.Width != storage.width || targetDescription.Height != storage.height
        || targetDescription.SampleDesc.Count != storage.sourceSamples
        || targetDescription.SampleDesc.Quality != storage.sourceQuality) {
        note("result=fail", "target_depth_sample_mismatch");
        return false;
    }
    const D3D11_VIEWPORT viewport{exactView.viewport.x,
                                  exactView.viewport.y,
                                  exactView.viewport.width,
                                  exactView.viewport.height,
                                  exactView.viewport.minimumDepth,
                                  exactView.viewport.maximumDepth};

    // Borrow every pipeline state the game had set so it can be handed back verbatim.
    ID3D11RenderTargetView* priorTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* priorDepth = nullptr;
    context->OMGetRenderTargets(
        static_cast<UINT>(std::size(priorTargets)), priorTargets, &priorDepth);

    ID3D11RasterizerState* priorRasterizer = nullptr;
    context->RSGetState(&priorRasterizer);
    D3D11_VIEWPORT priorViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = static_cast<UINT>(std::size(priorViewports));
    context->RSGetViewports(&viewportCount, priorViewports);
    D3D11_RECT priorScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scissorCount = static_cast<UINT>(std::size(priorScissors));
    context->RSGetScissorRects(&scissorCount, priorScissors);

    ID3D11InputLayout* priorLayout = nullptr;
    context->IAGetInputLayout(&priorLayout);
    ID3D11Buffer* priorVertexBuffer = nullptr;
    UINT priorStride = 0;
    UINT priorOffset = 0;
    context->IAGetVertexBuffers(0, 1, &priorVertexBuffer, &priorStride, &priorOffset);
    ID3D11Buffer* priorIndexBuffer = nullptr;
    DXGI_FORMAT priorIndexFormat = DXGI_FORMAT_UNKNOWN;
    UINT priorIndexOffset = 0;
    context->IAGetIndexBuffer(&priorIndexBuffer, &priorIndexFormat, &priorIndexOffset);
    D3D11_PRIMITIVE_TOPOLOGY priorTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&priorTopology);

    ID3D11VertexShader* priorVertexShader = nullptr;
    context->VSGetShader(&priorVertexShader, nullptr, nullptr);
    ID3D11Buffer* priorVertexConstants = nullptr;
    context->VSGetConstantBuffers(0, 1, &priorVertexConstants);
    ID3D11GeometryShader* priorGeometryShader = nullptr;
    context->GSGetShader(&priorGeometryShader, nullptr, nullptr);
    ID3D11Buffer* priorGeometryConstants = nullptr;
    context->GSGetConstantBuffers(0, 1, &priorGeometryConstants);
    ID3D11HullShader* priorHullShader = nullptr;
    context->HSGetShader(&priorHullShader, nullptr, nullptr);
    ID3D11DomainShader* priorDomainShader = nullptr;
    context->DSGetShader(&priorDomainShader, nullptr, nullptr);
    ID3D11PixelShader* priorPixelShader = nullptr;
    context->PSGetShader(&priorPixelShader, nullptr, nullptr);
    ID3D11Buffer* priorPixelConstants = nullptr;
    context->PSGetConstantBuffers(0, 1, &priorPixelConstants);
    ID3D11ShaderResourceView* priorPixelResource = nullptr;
    context->PSGetShaderResources(0, 1, &priorPixelResource);
    ID3D11BlendState* priorBlend = nullptr;
    FLOAT priorBlendFactors[4]{};
    UINT priorBlendMask = 0;
    context->OMGetBlendState(&priorBlend, priorBlendFactors, &priorBlendMask);
    ID3D11DepthStencilState* priorDepthState = nullptr;
    UINT priorStencilReference = 0;
    context->OMGetDepthStencilState(&priorDepthState, &priorStencilReference);

    constexpr UINT stride = sizeof(inspection::SceneVertex);
    constexpr UINT offset = 0;
    const UINT vertexCount = static_cast<UINT>(lineCount) * 2U;
    ID3D11RenderTargetView* targetArray[] = {target};
    context->OMSetRenderTargets(1, targetArray, storage.depthStencilView);
    context->OMSetDepthStencilState(storage.depthComparison == D3D11_COMPARISON_GREATER_EQUAL
                                        ? storage.reversedDepthState
                                        : storage.standardDepthState,
                                    0);
    context->OMSetBlendState(storage.blendState, nullptr, 0xFFFFFFFFU);
    context->RSSetState(storage.lineRasterizerState);
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(0, nullptr);
    context->IASetInputLayout(storage.inputLayout);
    context->IASetVertexBuffers(0, 1, &storage.vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    context->VSSetShader(storage.vertexShader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &storage.constantBuffer);
    context->GSSetShader(storage.lineGeometryShader, nullptr, 0);
    context->GSSetConstantBuffers(0, 1, &storage.constantBuffer);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(storage.pixelShader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &storage.constantBuffer);
    ID3D11ShaderResourceView* nullDepthResource = nullptr;
    context->PSSetShaderResources(0, 1, &nullDepthResource);
    std::size_t selectedLines = 0;
    while (selectedLines < lineCount && lines[selectedLines].selected) {
        ++selectedLines;
    }
    std::size_t hoveredLines = 0;
    while (selectedLines + hoveredLines < lineCount
           && lines[selectedLines + hoveredLines].hovered) {
        ++hoveredLines;
    }
    const auto updateConstants = [&](float halfWidth, float sizeAddition) noexcept {
        D3D11_MAPPED_SUBRESOURCE updated{};
        if (FAILED(context->Map(storage.constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &updated))) {
            return false;
        }
        auto* values = static_cast<std::array<float, 20>*>(updated.pData);
        std::memcpy(values->data(), matrix.data(), sizeof(matrix));
        (*values)[16U] = lineViewportWidth;
        (*values)[17U] = lineViewportHeight;
        (*values)[18U] = halfWidth;
        (*values)[19U] = sizeAddition;
        context->Unmap(storage.constantBuffer, 0);
        return true;
    };

    // Halos are submitted underneath the role-colored geometry. This keeps the
    // marker/bounds identity stable while selection remains visible through depth.
    if (selectedLines != 0 && storage.selectionHaloPixelShader != nullptr) {
        if (updateConstants(baseHalfWidth + 1.5F, 0.0F)) {
            context->OMSetDepthStencilState(storage.selectionHaloDepthState, 0);
            context->PSSetShader(storage.selectionHaloPixelShader, nullptr, 0);
            context->Draw(static_cast<UINT>(selectedLines) * 2U, 0);
        }
    }
    if (hoveredLines != 0 && storage.hoverPixelShader != nullptr
        && updateConstants(baseHalfWidth + 1.0F, 0.0F)) {
        context->OMSetDepthStencilState(storage.depthComparison == D3D11_COMPARISON_GREATER_EQUAL
                                            ? storage.reversedDepthState
                                            : storage.standardDepthState,
                                        0);
        context->PSSetShader(storage.hoverPixelShader, nullptr, 0);
        context->Draw(static_cast<UINT>(hoveredLines) * 2U, static_cast<UINT>(selectedLines) * 2U);
    }
    if (updateConstants(baseHalfWidth, 0.0F)) {
        context->OMSetDepthStencilState(storage.depthComparison == D3D11_COMPARISON_GREATER_EQUAL
                                            ? storage.reversedDepthState
                                            : storage.standardDepthState,
                                        0);
        context->PSSetShader(storage.pixelShader, nullptr, 0);
        context->Draw(vertexCount, 0);
    }

    if (glyphCount != 0) {
        constexpr UINT glyphStride = sizeof(GlyphVertex);
        context->IASetInputLayout(storage.glyphInputLayout);
        context->IASetVertexBuffers(0, 1, &storage.glyphVertexBuffer, &glyphStride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        context->VSSetShader(storage.glyphVertexShader, nullptr, 0);
        context->GSSetShader(storage.glyphGeometryShader, nullptr, 0);
        std::size_t selectedGlyphs = 0;
        while (selectedGlyphs < glyphCount && glyphs[selectedGlyphs].selected) {
            ++selectedGlyphs;
        }
        std::size_t hoveredGlyphs = 0;
        while (selectedGlyphs + hoveredGlyphs < glyphCount
               && glyphs[selectedGlyphs + hoveredGlyphs].hovered) {
            ++hoveredGlyphs;
        }
        if (selectedGlyphs != 0 && storage.selectionHaloPixelShader != nullptr
            && updateConstants(baseHalfWidth, 8.0F)) {
            context->OMSetDepthStencilState(storage.selectionHaloDepthState, 0);
            context->PSSetShader(storage.selectionHaloPixelShader, nullptr, 0);
            context->Draw(static_cast<UINT>(selectedGlyphs), 0);
        }
        if (hoveredGlyphs != 0 && storage.hoverPixelShader != nullptr
            && updateConstants(baseHalfWidth, 5.0F)) {
            context->OMSetDepthStencilState(storage.depthComparison
                                                    == D3D11_COMPARISON_GREATER_EQUAL
                                                ? storage.reversedDepthState
                                                : storage.standardDepthState,
                                            0);
            context->PSSetShader(storage.hoverPixelShader, nullptr, 0);
            context->Draw(static_cast<UINT>(hoveredGlyphs), static_cast<UINT>(selectedGlyphs));
        }
        if (updateConstants(baseHalfWidth, 0.0F)) {
            context->OMSetDepthStencilState(storage.depthComparison
                                                    == D3D11_COMPARISON_GREATER_EQUAL
                                                ? storage.reversedDepthState
                                                : storage.standardDepthState,
                                            0);
            context->PSSetShader(storage.pixelShader, nullptr, 0);
            context->Draw(static_cast<UINT>(glyphCount), 0);
        }
    }

    // Hand every pipeline state back exactly as the game left it.
    context->OMSetRenderTargets(
        static_cast<UINT>(std::size(priorTargets)), priorTargets, priorDepth);
    context->OMSetBlendState(priorBlend, priorBlendFactors, priorBlendMask);
    context->OMSetDepthStencilState(priorDepthState, priorStencilReference);
    context->RSSetState(priorRasterizer);
    context->RSSetViewports(viewportCount, priorViewports);
    context->RSSetScissorRects(scissorCount, priorScissors);
    context->IASetInputLayout(priorLayout);
    context->IASetVertexBuffers(0, 1, &priorVertexBuffer, &priorStride, &priorOffset);
    context->IASetIndexBuffer(priorIndexBuffer, priorIndexFormat, priorIndexOffset);
    context->IASetPrimitiveTopology(priorTopology);
    context->VSSetShader(priorVertexShader, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &priorVertexConstants);
    context->GSSetShader(priorGeometryShader, nullptr, 0);
    context->GSSetConstantBuffers(0, 1, &priorGeometryConstants);
    context->HSSetShader(priorHullShader, nullptr, 0);
    context->DSSetShader(priorDomainShader, nullptr, 0);
    context->PSSetShader(priorPixelShader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &priorPixelConstants);
    context->PSSetShaderResources(0, 1, &priorPixelResource);

    release_com(priorDepth);
    for (ID3D11RenderTargetView* renderTarget : priorTargets) {
        release_com(renderTarget);
    }
    release_com(priorRasterizer);
    release_com(priorLayout);
    release_com(priorVertexBuffer);
    release_com(priorIndexBuffer);
    release_com(priorVertexShader);
    release_com(priorVertexConstants);
    release_com(priorGeometryShader);
    release_com(priorGeometryConstants);
    release_com(priorHullShader);
    release_com(priorDomainShader);
    release_com(priorPixelShader);
    release_com(priorPixelConstants);
    release_com(priorPixelResource);
    release_com(priorBlend);
    release_com(priorDepthState);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    storage.lastDrawMicros = static_cast<std::uint64_t>(elapsed.count());
    storage.maximumDrawMicros = (std::max)(storage.maximumDrawMicros, storage.lastDrawMicros);
    storage.consecutiveFailures = 0;
    if (storage.lastDrawMicros >= kSlowOperationMicros && !storage.slowDrawReported) {
        storage.slowDrawReported = true;
        note("result=slow", "draw_over_budget");
    }
    return true;
}

inspection::PickingReadiness draw_pick_ids(ID3D11DeviceContext* context,
                                           const inspection::SceneFramePtr& batch,
                                           std::uint64_t capturedFrame,
                                           const inspection::RenderViewSnapshot& exactView,
                                           Storage& storage) noexcept {
    if (context == nullptr) {
        return inspection::PickingReadiness::unavailable;
    }
    const inspection::PickingReadiness readbackState = poll_pick_readbacks(context, storage);
    if (!batch || (batch->lines.empty() && batch->glyphs.empty())
        || storage.depthStencilView == nullptr) {
        return inspection::PickingReadiness::unavailable;
    }
    if (!exactView.valid || !exactView.exactNative || exactView.engineFrame == 0
        || exactView.viewport.width <= 0.0F || exactView.viewport.height <= 0.0F) {
        return inspection::PickingReadiness::waitingForView;
    }
    inspection::PickRequest request{};
    if (!load_pick_request(request) || request.sequence <= storage.lastPickRequest) {
        return readbackState;
    }
    const std::uint32_t viewportX = exactView.viewport.x >= 0.0F
                                        ? static_cast<std::uint32_t>(exactView.viewport.x)
                                        : storage.width;
    const std::uint32_t viewportY = exactView.viewport.y >= 0.0F
                                        ? static_cast<std::uint32_t>(exactView.viewport.y)
                                        : storage.height;
    if (request.graphGeneration != batch->graphGeneration || request.capturedFrame == 0
        || request.capturedFrame > capturedFrame || capturedFrame - request.capturedFrame > 2U
        || request.x < viewportX || request.y < viewportY || request.x - viewportX >= storage.width
        || request.y - viewportY >= storage.height) {
        storage.lastPickRequest = request.sequence;
        return readbackState;
    }

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr) {
        return inspection::PickingReadiness::unavailable;
    }
    const bool pipelineReady =
        storage.pickVertexShader != nullptr && storage.pickGeometryShader != nullptr
        && storage.pickGlyphVertexShader != nullptr && storage.pickGlyphGeometryShader != nullptr
        && storage.pickPixelShader != nullptr && storage.pickResolveShader != nullptr
        && storage.pickInputLayout != nullptr && storage.pickGlyphInputLayout != nullptr
        && storage.pickVertexBuffer != nullptr && storage.pickGlyphVertexBuffer != nullptr
        && storage.pickConstantBuffer != nullptr && storage.pickResolveConstantBuffer != nullptr
        && storage.pickDepthState != nullptr && storage.pickRasterizerState != nullptr;
    if (!pipelineReady) {
        release_pick_pipeline(storage);
    }
    const bool ready = (pipelineReady || create_pick_pipeline(device, storage))
                       && create_pick_targets(device, storage);
    device->Release();
    if (!ready) {
        return inspection::PickingReadiness::unavailable;
    }

    Storage::PickReadback* readback = nullptr;
    for (std::size_t offset = 0; offset < storage.pickReadbacks.size(); ++offset) {
        const std::size_t index =
            (storage.nextPickReadback + offset) % storage.pickReadbacks.size();
        if (!storage.pickReadbacks[index].pending) {
            readback = &storage.pickReadbacks[index];
            storage.nextPickReadback = (index + 1U) % storage.pickReadbacks.size();
            break;
        }
    }
    if (readback == nullptr) {
        return inspection::PickingReadiness::waitingForReadback;
    }

    const std::size_t lineCount = (std::min)(batch->lines.size(), inspection::kMaximumSceneLines);
    const std::size_t glyphCount =
        (std::min)(batch->glyphs.size(), static_cast<std::size_t>(kMaximumGlyphs));
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (lineCount != 0) {
        if (FAILED(
                context->Map(storage.pickVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return inspection::PickingReadiness::unavailable;
        }
        auto* vertices = static_cast<PickVertex*>(mapped.pData);
        for (std::size_t index = 0; index < lineCount; ++index) {
            vertices[index * 2U] = {batch->lines[index].first.position,
                                    batch->lines[index].pickToken};
            vertices[index * 2U + 1U] = {batch->lines[index].second.position,
                                         batch->lines[index].pickToken};
        }
        context->Unmap(storage.pickVertexBuffer, 0);
    }
    if (glyphCount != 0) {
        if (FAILED(context->Map(
                storage.pickGlyphVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return inspection::PickingReadiness::unavailable;
        }
        auto* glyphVertices = static_cast<PickGlyphVertex*>(mapped.pData);
        for (std::size_t index = 0; index < glyphCount; ++index) {
            glyphVertices[index] = {batch->glyphs[index].position,
                                    batch->glyphs[index].pickToken,
                                    static_cast<std::uint32_t>(batch->glyphs[index].shape),
                                    batch->glyphs[index].sizePixels};
        }
        context->Unmap(storage.pickGlyphVertexBuffer, 0);
    }

    if (FAILED(context->Map(storage.pickConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return inspection::PickingReadiness::unavailable;
    }
    auto* constants = static_cast<PickConstants*>(mapped.pData);
    constants->viewProjection = exactView.viewProjection;
    constants->viewportSize = {exactView.viewport.width, exactView.viewport.height};
    // Editor handles intentionally use a larger invisible hit proxy than their
    // visual stroke. Sixteen pixels keeps thin/far bounds practical to select.
    constants->halfWidth = 8.0F;
    constants->padding = 0.0F;
    context->Unmap(storage.pickConstantBuffer, 0);

    const std::uint32_t pickX = request.x - viewportX;
    const std::uint32_t pickY = request.y - viewportY;
    if (storage.sourceSamples > 1) {
        if (FAILED(context->Map(
                storage.pickResolveConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return inspection::PickingReadiness::unavailable;
        }
        *static_cast<PickResolveConstants*>(mapped.pData) = {
            pickX, pickY, storage.sourceSamples, 0};
        context->Unmap(storage.pickResolveConstantBuffer, 0);
    }

    PickPipelineState prior{};
    prior.capture(context);
    context->CopyResource(storage.pickDepthTexture, storage.depthTexture);
    constexpr std::array<float, 4> clearIds{};
    context->ClearRenderTargetView(storage.pickTarget, clearIds.data());
    ID3D11ShaderResourceView* nullPixelResources[2]{};
    context->PSSetShaderResources(0, 2, nullPixelResources);
    context->OMSetRenderTargets(1, &storage.pickTarget, storage.pickDepthStencilView);
    context->OMSetDepthStencilState(storage.pickDepthState, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    context->RSSetState(storage.pickRasterizerState);
    const D3D11_VIEWPORT viewport{0.0F,
                                  0.0F,
                                  exactView.viewport.width,
                                  exactView.viewport.height,
                                  exactView.viewport.minimumDepth,
                                  exactView.viewport.maximumDepth};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(0, nullptr);
    constexpr UINT stride = sizeof(PickVertex);
    constexpr UINT offset = 0;
    context->IASetInputLayout(storage.pickInputLayout);
    context->IASetVertexBuffers(0, 1, &storage.pickVertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    context->VSSetShader(storage.pickVertexShader, nullptr, 0);
    context->VSSetConstantBuffers(1, 1, &storage.pickConstantBuffer);
    context->GSSetShader(storage.pickGeometryShader, nullptr, 0);
    context->GSSetConstantBuffers(1, 1, &storage.pickConstantBuffer);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(storage.pickPixelShader, nullptr, 0);
    if (lineCount != 0) {
        context->Draw(static_cast<UINT>(lineCount) * 2U, 0);
    }
    if (glyphCount != 0) {
        constexpr UINT glyphStride = sizeof(PickGlyphVertex);
        context->IASetInputLayout(storage.pickGlyphInputLayout);
        context->IASetVertexBuffers(0, 1, &storage.pickGlyphVertexBuffer, &glyphStride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        context->VSSetShader(storage.pickGlyphVertexShader, nullptr, 0);
        context->GSSetShader(storage.pickGlyphGeometryShader, nullptr, 0);
        context->Draw(static_cast<UINT>(glyphCount), 0);
    }

    ID3D11RenderTargetView* nullTarget = nullptr;
    context->OMSetRenderTargets(1, &nullTarget, nullptr);
    if (storage.sourceSamples == 1) {
        const D3D11_BOX pixel{pickX, pickY, 0, pickX + 1U, pickY + 1U, 1};
        context->CopySubresourceRegion(
            readback->staging, 0, 0, 0, 0, storage.pickTexture, 0, &pixel);
    } else {
        context->CSSetShader(storage.pickResolveShader, nullptr, 0);
        context->CSSetConstantBuffers(2, 1, &storage.pickResolveConstantBuffer);
        context->CSSetShaderResources(1, 1, &storage.pickView);
        constexpr UINT keepCounter = D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
        context->CSSetUnorderedAccessViews(0, 1, &storage.pickResolvedView, &keepCounter);
        context->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* nullUnordered = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &nullUnordered, &keepCounter);
        ID3D11ShaderResourceView* nullResource = nullptr;
        context->CSSetShaderResources(1, 1, &nullResource);
        context->CopyResource(readback->staging, storage.pickResolved);
    }

    readback->batch = batch;
    readback->requestSequence = request.sequence;
    readback->capturedFrame = capturedFrame;
    readback->engineFrame = exactView.engineFrame;
    readback->viewPublication = exactView.publication;
    readback->depthSequence = storage.captureSequence;
    readback->graphGeneration = batch->graphGeneration;
    readback->pending = true;
    storage.lastPickRequest = request.sequence;
    prior.restore(context);
    prior.release();
    return inspection::PickingReadiness::waitingForReadback;
}

void release(Storage& storage) noexcept {
    release_objects(storage);
    release_pick_targets(storage);
    release_pick_pipeline(storage);
    for (Storage::PickReadback& readback : storage.pickReadbacks) {
        release_com(readback.staging);
        readback.batch.reset();
    }
    release_com(storage.vertexBuffer);
    release_com(storage.glyphVertexBuffer);
    release_com(storage.constantBuffer);
    release_com(storage.vertexShader);
    release_com(storage.lineGeometryShader);
    release_com(storage.glyphVertexShader);
    release_com(storage.glyphGeometryShader);
    release_com(storage.pixelShader);
    release_com(storage.selectionHaloPixelShader);
    release_com(storage.hoverPixelShader);
    release_com(storage.inputLayout);
    release_com(storage.glyphInputLayout);
    release_com(storage.lineRasterizerState);
    release_com(storage.blendState);
    release_com(storage.standardDepthState);
    release_com(storage.reversedDepthState);
    release_com(storage.selectionHaloDepthState);
    storage = {};
    inspection::clear_pick_state();
}

bool depth_available(const Storage& storage) noexcept {
    return storage.depthStencilView != nullptr && !storage.rejected
           && storage.capability == inspection::DepthCapability::supported;
}

} // namespace sunrise::client::hooks::graphics::renderer::debug_render
