#include "win32_clipboard.h"

#include <windows.h>

#include <shellapi.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cwchar>
#include <string>

namespace halocap::platform::win32 {

namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

bool CreateFactory(IWICImagingFactory** factory) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory)));
}

bool FrameToBgra(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
                 std::int64_t maxBytes, ClipboardImage* image) {
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }
    if (static_cast<std::int64_t>(width) * height * 4 > maxBytes) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return false;
    }
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);

    const UINT stride = width * 4;
    const UINT size = stride * height;
    if (FAILED(converter->CopyPixels(nullptr, stride, size, image->bgra.data()))) {
        image->bgra.clear();
        return false;
    }
    return true;
}

bool DecodeWithWic(IWICBitmapDecoder* decoder, IWICImagingFactory* factory,
                   std::int64_t maxBytes, ClipboardImage* image) {
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }
    return FrameToBgra(factory, frame.Get(), maxBytes, image);
}

bool ReadDib(HGLOBAL handle, std::int64_t maxBytes, ClipboardImage* image) {
    const auto* base = static_cast<const std::uint8_t*>(GlobalLock(handle));
    if (base == nullptr) {
        return false;
    }

    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base);
    bool ok = false;
    if (header->biSize >= sizeof(BITMAPINFOHEADER) && header->biWidth > 0 &&
        header->biHeight != 0 &&
        (header->biBitCount == 32 || header->biBitCount == 24)) {
        // V5 头带 alpha mask 时 alpha 是有效数据（含 0）；普通 CF_DIB 的 32 位
        // BI_RGB 里 alpha 字节常常全是 0，只是填充，不能当透明处理。
        bool alphaMeaningful = false;
        if (header->biSize >= sizeof(BITMAPV5HEADER)) {
            const auto* v5 = reinterpret_cast<const BITMAPV5HEADER*>(base);
            alphaMeaningful =
                header->biBitCount == 32 && v5->bV5AlphaMask == 0xFF000000u;
        }
        const int width = header->biWidth;
        const int height = std::abs(header->biHeight);
        const std::int64_t bytes = static_cast<std::int64_t>(width) * height * 4;
        if (bytes <= maxBytes) {
            const int stride = ((width * header->biBitCount + 31) / 32) * 4;
            const std::uint8_t* pixels = base + header->biSize;
            image->width = width;
            image->height = height;
            image->bgra.assign(static_cast<std::size_t>(bytes), 255);
            const bool bottomUp = header->biHeight > 0;
            bool anyAlpha = false;
            for (int y = 0; y < height; ++y) {
                const int sourceY = bottomUp ? height - 1 - y : y;
                const std::uint8_t* row =
                    pixels + static_cast<std::size_t>(sourceY) * stride;
                std::uint8_t* destination =
                    image->bgra.data() + static_cast<std::size_t>(y) * width * 4;
                if (header->biBitCount == 24) {
                    for (int x = 0; x < width; ++x) {
                        destination[x * 4 + 0] = row[x * 3 + 0];
                        destination[x * 4 + 1] = row[x * 3 + 1];
                        destination[x * 4 + 2] = row[x * 3 + 2];
                        destination[x * 4 + 3] = 255;
                    }
                } else {
                    std::memcpy(destination, row, static_cast<std::size_t>(width) * 4);
                    for (int x = 0; x < width; ++x) {
                        anyAlpha |= destination[x * 4 + 3] != 0;
                    }
                }
            }
            if (!anyAlpha && !alphaMeaningful) {
                // 32 位 DIB 的 alpha 全为 0：多半只是填充字节，按不透明处理。
                for (std::size_t i = 3; i < image->bgra.size(); i += 4) {
                    image->bgra[i] = 255;
                }
            }
            ok = true;
        }
    }

    GlobalUnlock(handle);
    return ok;
}

}  // namespace

bool ReadClipboard(ClipboardReadResult* result) {
    if (result == nullptr) {
        return false;
    }
    *result = ClipboardReadResult{};

    bool opened = false;
    for (int attempt = 0; attempt < 3 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != 0;
        if (!opened) {
            Sleep(20);
        }
    }
    if (!opened) {
        return false;
    }

    result->sequence = GetClipboardSequenceNumber();

    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HANDLE handle = GetClipboardData(CF_HDROP);
        if (handle != nullptr) {
            const auto drop = static_cast<HDROP>(handle);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (length == 0) {
                    continue;
                }
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1);
                path.resize(length);
                result->filePaths.push_back(WideToUtf8(path));
            }
        }
    }

    if (result->filePaths.empty()) {
        // PNG 优先于 CF_DIB（alpha 更完整）。
        const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        if (pngFormat != 0 && IsClipboardFormatAvailable(pngFormat)) {
            HANDLE handle = GetClipboardData(pngFormat);
            if (handle != nullptr) {
                const auto* bytes = static_cast<const std::uint8_t*>(GlobalLock(handle));
                const std::size_t size = GlobalSize(handle);
                if (bytes != nullptr) {
                    DecodeImageBytes(bytes, size, 256LL * 1024 * 1024, &result->image);
                    result->hasImage = !result->image.bgra.empty();
                    GlobalUnlock(handle);
                }
            }
        }

        if (!result->hasImage && IsClipboardFormatAvailable(CF_DIBV5)) {
            HANDLE handle = GetClipboardData(CF_DIBV5);
            if (handle != nullptr) {
                result->hasImage = ReadDib(handle, 256LL * 1024 * 1024, &result->image);
            }
        }

        if (!result->hasImage && IsClipboardFormatAvailable(CF_DIB)) {
            HANDLE handle = GetClipboardData(CF_DIB);
            if (handle != nullptr) {
                result->hasImage = ReadDib(handle, 256LL * 1024 * 1024, &result->image);
            }
        }
    }

    if (!result->hasImage && result->filePaths.empty()) {
        const UINT htmlFormat = RegisterClipboardFormatW(L"HTML Format");
        if (htmlFormat != 0 && IsClipboardFormatAvailable(htmlFormat)) {
            HANDLE handle = GetClipboardData(htmlFormat);
            if (handle != nullptr) {
                const auto* bytes = static_cast<const char*>(GlobalLock(handle));
                const std::size_t size = GlobalSize(handle);
                if (bytes != nullptr) {
                    result->html.assign(bytes, size);
                    while (!result->html.empty() && result->html.back() == '\0') {
                        result->html.pop_back();
                    }
                    result->hasHtml = !result->html.empty();
                    GlobalUnlock(handle);
                }
            }
        }
    }

    if (!result->hasImage && result->filePaths.empty()) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE handle = GetClipboardData(CF_UNICODETEXT);
            if (handle != nullptr) {
                const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
                if (text != nullptr) {
                    result->text = WideToUtf8(text);
                    result->hasText = true;
                    GlobalUnlock(handle);
                }
            }
        } else if (IsClipboardFormatAvailable(CF_TEXT)) {
            HANDLE handle = GetClipboardData(CF_TEXT);
            if (handle != nullptr) {
                const auto* text = static_cast<const char*>(GlobalLock(handle));
                if (text != nullptr) {
                    result->text = std::string(text);
                    result->hasText = true;
                    GlobalUnlock(handle);
                }
            }
        }
    }

    CloseClipboard();
    return true;
}

bool DecodeImageBytes(const std::uint8_t* bytes, std::size_t size, std::int64_t maxBytes,
                      ClipboardImage* image) {
    if (bytes == nullptr || size == 0 || image == nullptr) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (!CreateFactory(factory.GetAddressOf())) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(size)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }
    return DecodeWithWic(decoder.Get(), factory.Get(), maxBytes, image);
}

bool LoadImageFile(const std::string& utf8Path, std::int64_t maxBytes, ClipboardImage* image) {
    if (utf8Path.empty() || image == nullptr) {
        return false;
    }
    const std::wstring path = Utf8ToWide(utf8Path);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (!CreateFactory(factory.GetAddressOf())) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }
    return DecodeWithWic(decoder.Get(), factory.Get(), maxBytes, image);
}

namespace {

bool CreateDib(int width, int height, ClipboardImage* image) {
    image->width = width;
    image->height = height;
    image->bgra.assign(static_cast<std::size_t>(width) * height * 4, 255);
    return true;
}

void DrawTextOnDib(HDC dc, int width, int height, const std::wstring& text, double scale,
                   const RECT& bounds, COLORREF color, int fontSize) {
    HFONT font = CreateFontW(-static_cast<int>(fontSize * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT rect = bounds;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    (void)width;
    (void)height;
}

// 在 32bpp 的 DIB 上画：返回 BGRA 拷贝。cd 只用于控件文本，不是控件。
bool RenderWithGdi(int width, int height, const std::function<void(HDC, HDC)>& draw,
                   ClipboardImage* image) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = SelectObject(memory, bitmap);
    draw(screen, memory);

    image->width = width;
    image->height = height;
    image->bgra.assign(static_cast<std::uint8_t*>(bits),
                       static_cast<std::uint8_t*>(bits) +
                           static_cast<std::size_t>(width) * height * 4);
    // GDI 只写 RGB，DIB 的 alpha 字节保持 0；这些卡片本身是不透明的，必须补成 255，
    // 否则贴图窗口按 ARGB 渲染时会整张透明（文字/颜色贴图看不见）。
    for (std::size_t i = 3; i < image->bgra.size(); i += 4) {
        image->bgra[i] = 255;
    }

    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return true;
}

}  // namespace

bool RenderTextCard(const std::string& utf8Text, double scale, ClipboardImage* image) {
    if (utf8Text.empty() || image == nullptr) {
        return false;
    }
    std::string visible = utf8Text;
    if (visible.size() > 32768) {
        visible.resize(32768);
        visible += "…";
    }

    const core::clipboard::TextCardMetrics metrics =
        core::clipboard::TextCardBounds(visible, scale);
    return RenderWithGdi(metrics.width, metrics.height,
                         [&](HDC, HDC memory) {
                             RECT full{ 0, 0, metrics.width, metrics.height };
                             FillRect(memory, &full,
                                      static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                             const RECT bounds{ static_cast<int>(16 * scale),
                                                static_cast<int>(16 * scale),
                                                metrics.width - static_cast<int>(16 * scale),
                                                metrics.height - static_cast<int>(16 * scale) };
                             DrawTextOnDib(memory, metrics.width, metrics.height,
                                           Utf8ToWide(visible), scale, bounds, RGB(35, 41, 51),
                                           17);
                         },
                         image);
}

bool RenderColorCard(core::clipboard::Color color, double scale, ClipboardImage* image) {
    if (image == nullptr) {
        return false;
    }
    const int width = static_cast<int>(280 * scale);
    const int height = static_cast<int>(190 * scale);
    const int swatchHeight = static_cast<int>(120 * scale);

    wchar_t hex[16] = {};
    if (color.a < 255) {
        std::swprintf(hex, 16, L"#%02X%02X%02X%02X", color.r, color.g, color.b, color.a);
    } else {
        std::swprintf(hex, 16, L"#%02X%02X%02X", color.r, color.g, color.b);
    }
    wchar_t rgb[64] = {};
    std::swprintf(rgb, 64, L"RGB  %d, %d, %d", color.r, color.g, color.b);

    return RenderWithGdi(width, height,
                         [&](HDC, HDC memory) {
                             RECT full{ 0, 0, width, height };
                             FillRect(memory, &full,
                                      static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                             // 半透明颜色预览按白底混合，和 1.x 的 SolidColorBrush 观感一致。
                             const auto overWhite = [&](std::uint8_t channel) {
                                 return static_cast<std::uint8_t>(
                                     (channel * color.a + 255 * (255 - color.a)) / 255);
                             };
                             HBRUSH brush = CreateSolidBrush(RGB(overWhite(color.r),
                                                                overWhite(color.g),
                                                                overWhite(color.b)));
                             RECT swatch{ 0, 0, width, swatchHeight };
                             FillRect(memory, &swatch, brush);
                             DeleteObject(brush);

                             const RECT hexBounds{ static_cast<int>(16 * scale),
                                                   static_cast<int>(132 * scale),
                                                   width, static_cast<int>(162 * scale) };
                             DrawTextOnDib(memory, width, height, hex, scale, hexBounds,
                                           RGB(0, 0, 0), 18);
                             const RECT rgbBounds{ static_cast<int>(16 * scale),
                                                   static_cast<int>(160 * scale), width,
                                                   static_cast<int>(186 * scale) };
                             DrawTextOnDib(memory, width, height, rgb, scale, rgbBounds,
                                           RGB(90, 90, 90), 13);
                         },
                         image);
}

bool WriteUnicodeTextToClipboard(const std::string& utf8Text) {
    const std::wstring wide = Utf8ToWide(utf8Text);
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(target, wide.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

}
