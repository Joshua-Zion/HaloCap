#include "image_export.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace halocap::platform::win32 {

namespace {

bool CreateImagingFactory(IWICImagingFactory** outFactory) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(outFactory)));
}

}  // namespace

bool SaveBgraPng(const std::wstring& path, const std::uint8_t* bgra, int width, int height,
                 int stride) {
    if (path.empty() || bgra == nullptr || width <= 0 || height <= 0 || stride < width * 4) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (!CreateImagingFactory(factory.GetAddressOf())) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) {
        return false;
    }
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
        return false;
    }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) {
        return false;
    }
    if (FAILED(frame->Initialize(nullptr))) {
        return false;
    }
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        return false;
    }

    // 从内存位图写而不是直接 WritePixels：PNG 编码器可能会换像素格式，
    // WriteSource 会替我们做那次转换，WritePixels 不会。
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(stride), static_cast<UINT>(stride) * height,
            const_cast<BYTE*>(bgra), &bitmap))) {
        return false;
    }

    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) {
        return false;
    }
    if (FAILED(frame->Commit())) {
        return false;
    }
    return SUCCEEDED(encoder->Commit());
}

bool SaveBgraJpeg(const std::wstring& path, const std::uint8_t* bgra, int width, int height,
                  int stride, int quality) {
    if (path.empty() || bgra == nullptr || width <= 0 || height <= 0 || stride < width * 4) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (!CreateImagingFactory(factory.GetAddressOf())) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&frame, &options))) {
        return false;
    }
    if (options != nullptr) {
        PROPBAG2 bag{};
        bag.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
        options->Write(1, &bag, &value);
    }
    if (FAILED(frame->Initialize(options.Get())) ||
        FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(stride), static_cast<UINT>(stride) * height,
            const_cast<BYTE*>(bgra), &bitmap))) {
        return false;
    }

    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit())) {
        return false;
    }
    return SUCCEEDED(encoder->Commit());
}

bool CopyBgraToClipboard(void* owner, const std::uint8_t* bgra, int width, int height, int stride) {
    if (bgra == nullptr || width <= 0 || height <= 0 || stride < width * 4) {
        return false;
    }

    const std::size_t imageBytes = static_cast<std::size_t>(width) * height * 4;
    const std::size_t totalBytes = sizeof(BITMAPINFOHEADER) + imageBytes;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
    if (memory == nullptr) {
        return false;
    }

    auto* base = static_cast<std::uint8_t*>(GlobalLock(memory));
    if (base == nullptr) {
        GlobalFree(memory);
        return false;
    }

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = width;
    // DIB 是底朝上的（正高度 = 从下往上存），所以逐行倒着拷。
    header.biHeight = height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(imageBytes);

    std::memcpy(base, &header, sizeof(header));
    std::uint8_t* destination = base + sizeof(header);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* source = bgra + static_cast<std::size_t>(height - 1 - y) * stride;
        std::memcpy(destination + static_cast<std::size_t>(y) * width * 4, source,
                    static_cast<std::size_t>(width) * 4);
    }

    GlobalUnlock(memory);

    if (!OpenClipboard(static_cast<HWND>(owner))) {
        GlobalFree(memory);
        return false;
    }

    EmptyClipboard();
    // 成功之后内存归剪贴板所有，这里不能再 GlobalFree。
    if (SetClipboardData(CF_DIB, memory) == nullptr) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    CloseClipboard();
    return true;
}

}
