#include "pin_window.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapPinWindow";
constexpr int kBorderPx = 1;
constexpr int kGlowPx = 10;

}  // namespace

PinWindow::PinWindow() = default;

PinWindow::~PinWindow() { Close(); }

bool PinWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.style = CS_DBLCLKS;   // 双击关闭要用
        description.lpfnWndProc = &PinWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool PinWindow::Create(const std::uint8_t* bgra, int width, int height, int screenX, int screenY) {
    if (bgra == nullptr || width <= 0 || height <= 0 || !EnsureClass()) {
        return false;
    }

    const int outerWidth = width + 2 * (kBorderPx + kGlowPx);
    const int outerHeight = height + 2 * (kBorderPx + kGlowPx);

    // 分层窗口的位图是预乘 alpha 的 BGRA：内容不透明、白边、外圈光晕向外渐隐。
    std::vector<std::uint8_t> layered(static_cast<std::size_t>(outerWidth) * outerHeight * 4, 0);

    const int contentLeft = kGlowPx + kBorderPx;
    const int contentTop = kGlowPx + kBorderPx;
    const int borderLeft = kGlowPx;
    const int borderTop = kGlowPx;
    const int borderRight = kGlowPx + kBorderPx + width + kBorderPx;
    const int borderBottom = kGlowPx + kBorderPx + height + kBorderPx;

    for (int y = 0; y < outerHeight; ++y) {
        for (int x = 0; x < outerWidth; ++x) {
            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            std::uint8_t a = 0;

            const bool insideBorder = x >= borderLeft && x < borderRight && y >= borderTop &&
                                      y < borderBottom;
            const bool insideContent = x >= contentLeft && x < contentLeft + width &&
                                       y >= contentTop && y < contentTop + height;

            if (insideContent) {
                const std::size_t source =
                    (static_cast<std::size_t>(y - contentTop) * width + (x - contentLeft)) * 4;
                b = bgra[source + 0];
                g = bgra[source + 1];
                r = bgra[source + 2];
                a = 255;
            } else if (insideBorder) {
                // 1px 白边：贴图在深色背景上也要有清晰轮廓。
                b = 255;
                g = 255;
                r = 255;
                a = 255;
            } else {
                // 光晕：到边框的切比雪夫距离越近越不透明，颜色取冷灰蓝。
                const int dx = std::max({ borderLeft - x, x - (borderRight - 1), 0 });
                const int dy = std::max({ borderTop - y, y - (borderBottom - 1), 0 });
                const int distance = std::max(dx, dy);
                if (distance < kGlowPx) {
                    const float t = 1.0f - static_cast<float>(distance) / kGlowPx;
                    const auto alpha = static_cast<std::uint8_t>(90.0f * t * t);
                    b = 60;
                    g = 90;
                    r = 120;
                    a = alpha;
                }
            }

            const std::size_t destination = (static_cast<std::size_t>(y) * outerWidth + x) * 4;
            layered[destination + 0] = static_cast<std::uint8_t>(b * a / 255);
            layered[destination + 1] = static_cast<std::uint8_t>(g * a / 255);
            layered[destination + 2] = static_cast<std::uint8_t>(r * a / 255);
            layered[destination + 3] = a;
        }
    }

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = outerWidth;
    info.bmiHeader.biHeight = -outerHeight;   // 顶朝下
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
    std::memcpy(bits, layered.data(), layered.size());
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    window_ = CreateWindowExW(exStyle, kClassName, L"HaloCap pin", WS_POPUP, screenX - contentLeft,
                              screenY - contentTop, outerWidth, outerHeight, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    POINT destination{ screenX - contentLeft, screenY - contentTop };
    SIZE size{ outerWidth, outerHeight };
    POINT sourcePoint{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(static_cast<HWND>(window_), screen, &destination, &size, memory, &sourcePoint, 0,
                        &blend, ULW_ALPHA);

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    ShowWindow(static_cast<HWND>(window_), SW_SHOWNOACTIVATE);
    contentWidth_ = width;
    contentHeight_ = height;
    return true;
}

void PinWindow::Close() {
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
        window_ = nullptr;
    }
}

LRESULT CALLBACK PinWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<PinWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = window;
        }
    }

    if (self != nullptr) {
        return self->Handle(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT PinWindow::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN: {
        dragging_ = true;
        GetCursorPos(&dragOrigin_);
        RECT rect{};
        GetWindowRect(window, &rect);
        dragWindow_ = POINT{ rect.left, rect.top };
        SetCapture(window);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (dragging_) {
            POINT cursor{};
            GetCursorPos(&cursor);
            const int x = dragWindow_.x + (cursor.x - dragOrigin_.x);
            const int y = dragWindow_.y + (cursor.y - dragOrigin_.y);
            SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        // 双击关闭（1.x 是悬停出现关闭按钮；分层窗口上先给一个不必瞄准的手势）。
        Close();
        return 0;

    case WM_DESTROY:
        window_ = nullptr;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}
