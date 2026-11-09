#include "desktop_grab.h"

#include <windows.h>

namespace halocap::platform::win32 {

namespace {

bool Grab(int left, int top, int width, int height, DesktopImage& image) {
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
    return Grab(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), image);
}

bool GrabVirtualDesktop(DesktopImage& image) {
    const DesktopBounds bounds = VirtualDesktopBounds();
    return Grab(bounds.left, bounds.top, bounds.width, bounds.height, image);
}

}
