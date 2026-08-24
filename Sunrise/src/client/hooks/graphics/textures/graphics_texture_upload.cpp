/**
 * Upload of bundled images into renderer textures. The interface draws them, so it owns the
 * published identifiers, and this layer owns the objects behind them.
 */

#include "graphics_texture_upload.h"

#include <Windows.h>

#include <string>
#include <string_view>
#include <wincodec.h>

#include "../../../../../resources/resource.h"
#include "../../../../core/logging/log.h"
#include "../../../../core/ui/textures/ui_texture_slots.h"

namespace sunrise::client::hooks::graphics::textures {
namespace {

/** A PNG holds one image, so every bundled image uses the decoder's first frame. */
constexpr UINT kImageFrameIndex = 0;
/** One mip level and one array slice: the sheet is drawn at its own scale. */
constexpr UINT kSingleLevel = 1;
/** The texture is not multisampled, which is quality level 0 of one sample. */
constexpr UINT kSingleSample = 1;
constexpr UINT kDefaultQuality = 0;
/** A 2D upload has no slice pitch. */
constexpr UINT kNoSlicePitch = 0;

/** @param object COM object we own. Released and cleared when set. */
template <typename Interface> void release_com(Interface*& object) noexcept {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

/** @param line Whole log line. @return Always false, so callers can return it directly. */
[[nodiscard]] bool fail(std::string_view line) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::warn, line);
    return false;
}

/** Balances one COM initialization on the calling thread. */
class ComScope final {
public:
    ComScope() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    /** Undoes only an initialization this scope performed. */
    ~ComScope() noexcept {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    /** @return True when COM can be called here, including a thread that already entered it. */
    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{};
};

/** @return The module holding the Sunrise resources, resolved from this code's own address. */
[[nodiscard]] HMODULE owning_module() noexcept {
    HMODULE module = nullptr;
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&owning_module),
                             &module);
    return module;
}

/**
 * Borrows one bundled image out of the module resources.
 * @param resourceId Numeric RCDATA identifier.
 * @param bytes Receives the resource bytes, owned by the module.
 * @param size Receives the resource size.
 * @return True when the resource is present and not empty.
 */
[[nodiscard]] bool bundled_image(int resourceId, const void*& bytes, DWORD& size) noexcept {
    const HMODULE module = owning_module();
    const HRSRC resource = module != nullptr
                               ? FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA)
                               : nullptr;
    if (resource == nullptr) {
        return false;
    }
    const DWORD resourceSize = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* resourceBytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (resourceSize == 0 || resourceBytes == nullptr) {
        return false;
    }
    bytes = resourceBytes;
    size = resourceSize;
    return true;
}

/**
 * Decodes one bundled PNG into a bitmap with red, green, blue and alpha bytes.
 * @param factory Imaging factory used for every decode step.
 * @param resourceId Numeric RCDATA identifier.
 * @param output Receives the decoded bitmap. Null when any step fails.
 * @return True when the bitmap is decoded.
 */
[[nodiscard]] bool
decode_image(IWICImagingFactory* factory, int resourceId, IWICBitmap*& output) noexcept {
    const void* bytes = nullptr;
    DWORD size = 0;
    if (!bundled_image(resourceId, bytes, size)) {
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* converted = nullptr;
    // The stream only reads the resource, which the module maps read-only.
    auto* mutableBytes = static_cast<WICInProcPointer>(const_cast<void*>(bytes));
    const bool decoded =
        SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromMemory(mutableBytes, size))
        && SUCCEEDED(factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(decoder->GetFrame(kImageFrameIndex, &frame))
        && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted))
        && SUCCEEDED(factory->CreateBitmapFromSource(converted, WICBitmapCacheOnLoad, &output));
    release_com(converted);
    release_com(frame);
    release_com(decoder);
    release_com(stream);
    return decoded;
}

/**
 * Creates one immutable texture and its view straight from the locked bitmap pixels.
 * @param device Device that creates and owns both objects.
 * @param bitmap Decoded bitmap, locked for reading here.
 * @param output Receives both objects. Left alone when any step fails.
 * @return True when the texture and its view exist.
 */
[[nodiscard]] bool
create_texture(ID3D11Device* device, IWICBitmap* bitmap, Uploaded& output) noexcept {
    UINT width = 0;
    UINT height = 0;
    if (FAILED(bitmap->GetSize(&width, &height))) {
        return false;
    }
    const WICRect area{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    IWICBitmapLock* lock = nullptr;
    if (FAILED(bitmap->Lock(&area, WICBitmapLockRead, &lock))) {
        return false;
    }

    UINT stride = 0;
    UINT pixelBytes = 0;
    BYTE* pixels = nullptr;
    Uploaded created{};
    bool uploaded = SUCCEEDED(lock->GetStride(&stride))
                    && SUCCEEDED(lock->GetDataPointer(&pixelBytes, &pixels)) && pixels != nullptr;
    if (uploaded) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = kSingleLevel;
        description.ArraySize = kSingleLevel;
        // Byte order matches the decoded format, so the pixels upload with no conversion.
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = kSingleSample;
        description.SampleDesc.Quality = kDefaultQuality;
        // The sheet never changes after upload, so the driver may place it where it likes.
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{pixels, stride, kNoSlicePitch};
        uploaded =
            SUCCEEDED(device->CreateTexture2D(&description, &initial, &created.texture))
            && SUCCEEDED(device->CreateShaderResourceView(created.texture, nullptr, &created.view));
    }
    release_com(lock);
    if (!uploaded) {
        release_com(created.view);
        release_com(created.texture);
        return false;
    }
    output = created;
    return true;
}

[[nodiscard]] bool upload_bundled_image(ID3D11Device* device,
                                        int resourceId,
                                        core::ui::textures::Slot slot,
                                        std::string_view stage,
                                        Uploaded& output) noexcept {
    if (device == nullptr) {
        return false;
    }
    const auto failure = [stage](std::string_view reason) noexcept {
        std::string line("ev=texture stage=");
        line.append(stage);
        line.append(" result=fail reason=");
        line.append(reason);
        return fail(line);
    };

    const ComScope com;
    if (!com.usable()) {
        return failure("com");
    }
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return failure("factory");
    }
    IWICBitmap* bitmap = nullptr;
    const bool decoded = decode_image(factory, resourceId, bitmap);
    release_com(factory);
    if (!decoded) {
        return failure("decode");
    }
    const bool created = create_texture(device, bitmap, output);
    release_com(bitmap);
    if (!created) {
        return failure("texture");
    }

    core::ui::textures::publish(slot, reinterpret_cast<ImTextureID>(output.view));
    std::string line("ev=texture stage=");
    line.append(stage);
    line.append(" result=ok");
    core::log::write(core::log::Channel::client, core::log::Level::debug, line);
    return true;
}

void release_bundled_image(core::ui::textures::Slot slot, Uploaded& uploaded) noexcept {
    if (uploaded.view != nullptr) {
        core::ui::textures::publish(slot, ImTextureID_Invalid);
    }
    release_com(uploaded.view);
    release_com(uploaded.texture);
}

} // namespace

/** Decodes the bundled logo sheet and publishes its view to the Core interface. */
bool upload_logo_sheet(ID3D11Device* device, Uploaded& output) noexcept {
    return upload_bundled_image(
        device, IDR_LOGO_SHEET, core::ui::textures::Slot::logoSheet, "logo_sheet", output);
}

/** Decodes the bundled inspector icon and publishes its view to the Core interface. */
bool upload_inspector_icon(ID3D11Device* device, Uploaded& output) noexcept {
    return upload_bundled_image(device,
                                IDR_INSPECTOR_ICON,
                                core::ui::textures::Slot::inspectorIcon,
                                "inspector_icon",
                                output);
}

/** Empties the published logo slot, then releases both objects. */
void release_logo_sheet(Uploaded& uploaded) noexcept {
    release_bundled_image(core::ui::textures::Slot::logoSheet, uploaded);
}

/** Empties the published inspector-icon slot, then releases both objects. */
void release_inspector_icon(Uploaded& uploaded) noexcept {
    release_bundled_image(core::ui::textures::Slot::inspectorIcon, uploaded);
}

} // namespace sunrise::client::hooks::graphics::textures
