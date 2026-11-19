#include "desktop_grab.h"

#include <windows.h>

#include <cstring>

namespace halocap::platform::win32 {

namespace {

bool Grab(int left, int top, int width, int height, DesktopImage& image,
          const std::vector<LayeredOverlay>* overlays) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }

    HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   // 顶朝下，和显示列表的坐标系一致
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

    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    // 屏幕 DC 的原点是主屏左上角，所以多屏要靠源坐标偏到虚拟桌面左上角。
    const BOOL copied =
        BitBlt(memory, 0, 0, width, height, screen, left, top, SRCCOPY | CAPTUREBLT);

    if (copied && overlays != nullptr) {
        // 分层窗口（托盘菜单、光圈）不进 BitBlt 的屏幕 DC，这里把调用方已经渲染好
        // 的预乘 alpha 位图用 AlphaBlend 补叠到抓屏结果上。left/top 是虚拟桌面原点，
        // overlay 的 screenX/screenY 也是虚拟桌面坐标，所以落点要减掉 left/top。
        for (const LayeredOverlay& overlay : *overlays) {
            if (overlay.width <= 0 || overlay.height <= 0 ||
                overlay.premultipliedBgra.size() <
                    static_cast<std::size_t>(overlay.width) * overlay.height * 4) {
                continue;
            }
            HDC overlayDc = CreateCompatibleDC(screen);
            if (overlayDc == nullptr) {
                continue;
            }
            BITMAPINFO overlayInfo{};
            overlayInfo.bmiHeader.biSize = sizeof(overlayInfo.bmiHeader);
            overlayInfo.bmiHeader.biWidth = overlay.width;
            overlayInfo.bmiHeader.biHeight = -overlay.height;
            overlayInfo.bmiHeader.biPlanes = 1;
            overlayInfo.bmiHeader.biBitCount = 32;
            overlayInfo.bmiHeader.biCompression = BI_RGB;
            void* overlayBits = nullptr;
            HBITMAP overlayBitmap =
                CreateDIBSection(overlayDc, &overlayInfo, DIB_RGB_COLORS, &overlayBits, nullptr, 0);
            if (overlayBitmap != nullptr && overlayBits != nullptr) {
                std::memcpy(overlayBits, overlay.premultipliedBgra.data(),
                            overlay.premultipliedBgra.size());
                HGDIOBJ oldOverlay = SelectObject(overlayDc, overlayBitmap);
                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = 255;
                blend.AlphaFormat = AC_SRC_ALPHA;
                AlphaBlend(memory, overlay.screenX - left, overlay.screenY - top, overlay.width,
                           overlay.height, overlayDc, 0, 0, overlay.width, overlay.height, blend);
                SelectObject(overlayDc, oldOverlay);
                DeleteObject(overlayBitmap);
            }
            DeleteDC(overlayDc);
        }
    }

    if (copied) {
        image.width = width;
        image.height = height;
        image.bgra.assign(static_cast<const std::uint8_t*>(bits),
                          static_cast<const std::uint8_t*>(bits) +
                              static_cast<std::size_t>(width) * height * 4);
        // BitBlt 给的是 BGRX，alpha 字节可能是 0；这里统一成不透明。
        for (std::size_t i = 3; i < image.bgra.size(); i += 4) {
            image.bgra[i] = 255;
        }
    }

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return copied != FALSE;
}

}  // namespace

DesktopBounds VirtualDesktopBounds() {
    DesktopBounds bounds;
    bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (bounds.width <= 0 || bounds.height <= 0) {
        bounds.left = 0;
        bounds.top = 0;
        bounds.width = GetSystemMetrics(SM_CXSCREEN);
        bounds.height = GetSystemMetrics(SM_CYSCREEN);
    }
    return bounds;
}

bool GrabPrimaryDesktop(DesktopImage& image) {
    return Grab(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), image,
                nullptr);
}

bool GrabVirtualDesktop(DesktopImage& image, const std::vector<LayeredOverlay>* overlays) {
    const DesktopBounds bounds = VirtualDesktopBounds();
    return Grab(bounds.left, bounds.top, bounds.width, bounds.height, image, overlays);
}

}
