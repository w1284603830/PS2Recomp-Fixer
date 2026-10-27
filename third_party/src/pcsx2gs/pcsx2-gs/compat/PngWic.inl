// PNG loading for texture replacement with the Windows Imaging Component instead of libpng (included by the generated
// compat/GSTextureReplacementLoaders.cpp in place of PCSX2's libpng handlers; tools/make_loaders.py).
// Same output as PCSX2's PNGLoader: 32-bit RGBA rows; images without alpha get A = 0x80 (opaque in GS terms), images
// with alpha keep it as stored. PCSX2 is GPL-3; so is this file.

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    // One factory per thread (texture loads run on the GS thread and on PCSX2's replacement worker thread).
    IWICImagingFactory *WicFactory()
    {
        thread_local Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        thread_local bool tried = false;
        if (!tried)
        {
            tried = true;
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            (void)init; // S_FALSE / RPC_E_CHANGED_MODE: COM is already usable on this thread
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        }
        return factory.Get();
    }

    bool WicHasAlpha(IWICImagingFactory *factory, const WICPixelFormatGUID &format)
    {
        Microsoft::WRL::ComPtr<IWICComponentInfo> info;
        Microsoft::WRL::ComPtr<IWICPixelFormatInfo2> pixelInfo;
        BOOL transparency = FALSE;
        if (FAILED(factory->CreateComponentInfo(format, info.GetAddressOf())) || FAILED(info.As(&pixelInfo)) ||
            FAILED(pixelInfo->SupportsTransparency(&transparency)))
            return true;
        return transparency != FALSE;
    }
}

bool PNGLoader(GSTextureReplacements::File& file, const char* filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
    IWICImagingFactory *factory = WicFactory();
    const s64 size = file.Size();
    if (!factory || size <= 0 || size > 0x7FFFFFFF || !file.Seek(0))
        return false;
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!file.Read(bytes.data(), bytes.size()))
        return false;

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    WICPixelFormatGUID sourceFormat{};
    UINT width = 0, height = 0;
    if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(frame->GetSize(&width, &height)) || width == 0 ||
        height == 0 || FAILED(frame->GetPixelFormat(&sourceFormat)) ||
        FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
    {
        Console.Error("Failed to decode replacement texture %s", filename);
        return false;
    }

    const u32 pitch = width * sizeof(u32);
    tex->width = width;
    tex->height = height;
    tex->format = GSTexture::Format::Color;
    tex->pitch = pitch;
    tex->data.resize(static_cast<size_t>(pitch) * height);
    if (FAILED(converter->CopyPixels(nullptr, pitch, static_cast<UINT>(tex->data.size()), tex->data.data())))
        return false;

    if (!WicHasAlpha(factory, sourceFormat))
    {
        u8 *pixel = tex->data.data();
        for (size_t i = 0, count = static_cast<size_t>(width) * height; i < count; ++i, pixel += 4)
            pixel[3] = 0x80;
    }
    return true;
}

// [DoD] Texture dumping (texture_dump=1, diagnostics only): 32-bit RGBA rows as PCSX2's libpng writer, alpha as
// stored (0x80 = opaque), so dumps pair with the replacement pack by name. WIC's PNG encoder may only take BGRA.
bool GSTextureReplacements::SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch)
{
    IWICImagingFactory *factory = WicFactory();
    if (!factory || width == 0 || height == 0)
        return false;
    using Microsoft::WRL::ComPtr;
    const int wide = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    std::wstring path(wide > 0 ? wide - 1 : 0, L'\0');
    if (wide <= 0 || !MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, path.data(), wide))
        return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(factory->CreateStream(stream.GetAddressOf())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)))
        return false;
    const bool bgra = IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) != FALSE;
    if (!bgra && !IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA))
        return false;
    std::vector<u8> rows(static_cast<size_t>(width) * 4 * height);
    for (u32 y = 0; y < height; ++y)
    {
        const u8 *src = buffer + static_cast<size_t>(y) * pitch;
        u8 *dst = rows.data() + static_cast<size_t>(y) * width * 4;
        std::memcpy(dst, src, static_cast<size_t>(width) * 4);
        if (bgra)
            for (u32 x = 0; x < width; ++x)
                std::swap(dst[x * 4], dst[x * 4 + 2]);
    }
    return SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(rows.size()), rows.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

