#include "graphics_frame_capture.h"

#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"

namespace sunrise::client::hooks::graphics::renderer::frame_capture {
namespace {

/** @param object COM object owned by the capture. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

void release_objects(Storage& storage) noexcept {
    release_com(storage.view);
    release_com(storage.texture);
}

[[nodiscard]] DXGI_FORMAT shader_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] bool descriptor_matches(const Storage& storage,
                                      const D3D11_TEXTURE2D_DESC& source,
                                      DXGI_FORMAT viewFormat) noexcept {
    return storage.width == source.Width && storage.height == source.Height
           && storage.sourceFormat == source.Format && storage.viewFormat == viewFormat
           && storage.sourceSamples == source.SampleDesc.Count;
}

void report_failure(const char* reason, const D3D11_TEXTURE2D_DESC& source) noexcept {
    std::array<char, 192> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=renderer stage=frame_capture result=fail reason=%s "
                                      "width=%u height=%u format=%u samples=%u",
                                      reason,
                                      source.Width,
                                      source.Height,
                                      static_cast<unsigned>(source.Format),
                                      source.SampleDesc.Count);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool create(ID3D11Device* device,
                          const D3D11_TEXTURE2D_DESC& source,
                          DXGI_FORMAT viewFormat,
                          Storage& storage) noexcept {
    D3D11_TEXTURE2D_DESC target{};
    target.Width = source.Width;
    target.Height = source.Height;
    target.MipLevels = 1;
    target.ArraySize = 1;
    target.Format = viewFormat;
    target.SampleDesc.Count = 1;
    target.SampleDesc.Quality = 0;
    target.Usage = D3D11_USAGE_DEFAULT;
    target.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = viewFormat;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MostDetailedMip = 0;
    view.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateTexture2D(&target, nullptr, &storage.texture))
        || FAILED(device->CreateShaderResourceView(storage.texture, &view, &storage.view))) {
        release_objects(storage);
        return false;
    }
    return true;
}

} // namespace

bool update(ID3D11Device* device,
            ID3D11DeviceContext* context,
            IDXGISwapChain* swapChain,
            Storage& storage) noexcept {
    if (device == nullptr || context == nullptr || swapChain == nullptr) {
        return false;
    }

    ID3D11Texture2D* source = nullptr;
    if (FAILED(swapChain->GetBuffer(
            0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source)))
        || source == nullptr) {
        release_com(source);
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    const DXGI_FORMAT viewFormat = shader_format(description.Format);
    if (!descriptor_matches(storage, description, viewFormat)) {
        release(storage);
        storage.width = description.Width;
        storage.height = description.Height;
        storage.sourceFormat = description.Format;
        storage.viewFormat = viewFormat;
        storage.sourceSamples = description.SampleDesc.Count;
    }

    if (storage.descriptorRejected) {
        release_com(source);
        return false;
    }
    if (viewFormat == DXGI_FORMAT_UNKNOWN || description.Width == 0 || description.Height == 0
        || description.SampleDesc.Count == 0 || description.ArraySize == 0) {
        storage.descriptorRejected = true;
        report_failure("unsupported_descriptor", description);
        release_com(source);
        return false;
    }
    if (storage.texture == nullptr || storage.view == nullptr) {
        if (!create(device, description, viewFormat, storage)) {
            storage.descriptorRejected = true;
            report_failure("create", description);
            release_com(source);
            return false;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         "ev=renderer stage=frame_capture result=ok");
    }

    if (description.SampleDesc.Count > 1) {
        context->ResolveSubresource(storage.texture, 0, source, 0, viewFormat);
    } else {
        context->CopySubresourceRegion(storage.texture, 0, 0, 0, 0, source, 0, nullptr);
    }
    release_com(source);
    return true;
}

void release(Storage& storage) noexcept {
    release_objects(storage);
    storage = {};
}

View view(const Storage& storage) noexcept {
    if (storage.view == nullptr || storage.texture == nullptr || storage.width == 0
        || storage.height == 0) {
        return {};
    }
    return {reinterpret_cast<ImTextureID>(storage.view),
            static_cast<float>(storage.width),
            static_cast<float>(storage.height)};
}

} // namespace sunrise::client::hooks::graphics::renderer::frame_capture
