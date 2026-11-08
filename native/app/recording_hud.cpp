#include "recording_hud.h"

#include <windowsx.h>

#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kCountdownClass[] = L"HaloCapRecordingCountdown";
constexpr wchar_t kHudClass[] = L"HaloCapRecordingHud";

bool EnsureGdiplus() {
    static ULONG_PTR token = 0;
    if (token == 0) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            return false;
        }
    }
    return true;
}

bool CreateLayeredBitmap(HDC* memory, HBITMAP* bitmap, void** bits, int width, int height) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (dib == nullptr || *bits == nullptr) {
        DeleteDC(dc);
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        return false;
    }
    SelectObject(dc, dib);
    *memory = dc;
    *bitmap = dib;
    return true;
}

std::wstring FormatTime(std::int64_t elapsedMs) {
    const std::int64_t seconds = std::max<std::int64_t>(0, elapsedMs / 1000);
    wchar_t buffer[16] = {};
    std::swprintf(buffer, 16, L"%02lld:%02lld", seconds / 60, seconds % 60);
    return buffer;
}

}  // namespace

bool RecordingCountdown::Show(int seconds) {
    if (!EnsureGdiplus() || seconds <= 0) {
        return true;
    }
    if (window_ != nullptr) {
        return true;
    }

    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &RecordingCountdown::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kCountdownClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) {
        return true;
    }

    width_ = 300;
    height_ = 300;
    if (!CreateLayeredBitmap(&memory_, &bitmap_, &bits_, width_, height_)) {
        return true;
    }

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screenWidth - width_) / 2;
    const int y = (screenHeight - height_) / 2;

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kCountdownClass, L"HaloCap countdown", WS_POPUP, x, y, width_,
                              height_, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return true;
    }

    bool cancelled = false;
    for (int left = seconds; left > 0; --left) {
        Render(left);
        ShowWindow(window_, SW_SHOWNOACTIVATE);

        const ULONGLONG deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                cancelled = true;
                break;
            }
            Sleep(30);
        }
        if (cancelled) {
            break;
        }
    }

    DestroyWindow(window_);
    window_ = nullptr;
    return !cancelled;
}

void RecordingCountdown::Render(int secondsLeft) {
    if (bits_ == nullptr) {
        return;
    }

    auto* canvas = new Gdiplus::Bitmap(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                                       static_cast<BYTE*>(bits_));
    Gdiplus::Graphics g(canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float cx = width_ * 0.5f;
    const float cy = height_ * 0.5f;
    Gdiplus::SolidBrush background(Gdiplus::Color(235, 0x16, 0x1A, 0x22));
    g.FillEllipse(&background, cx - 130.0f, cy - 130.0f, 260.0f, 260.0f);
    Gdiplus::Pen ring(Gdiplus::Color(90, 0x2F, 0x7C, 0xF6), 3.0f);
    g.DrawEllipse(&ring, cx - 130.0f, cy - 130.0f, 260.0f, 260.0f);

    Gdiplus::Font numberFont(L"Segoe UI", 96.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    const std::wstring number = std::to_wstring(secondsLeft);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
    g.DrawString(number.c_str(), static_cast<int>(number.size()), &numberFont,
                 Gdiplus::RectF(cx - 120.0f, cy - 110.0f, 240.0f, 150.0f), &format, &textBrush);

    Gdiplus::Font hintFont(L"Microsoft YaHei UI", 13.0f, Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
    Gdiplus::SolidBrush hintBrush(Gdiplus::Color(255, 0x9A, 0xA5, 0xB5));
    g.DrawString(L"录制即将开始 · Esc 取消", 12, &hintFont,
                 Gdiplus::RectF(cx - 120.0f, cy + 52.0f, 240.0f, 24.0f), &format, &hintBrush);

    delete canvas;
    HDC screen = GetDC(nullptr);
    POINT destination{ (GetSystemMetrics(SM_CXSCREEN) - width_) / 2,
                       (GetSystemMetrics(SM_CYSCREEN) - height_) / 2 };
    SIZE size{ width_, height_ };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(window_, screen, &destination, &size, memory_, &source, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK RecordingCountdown::WndProc(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    auto* self = reinterpret_cast<RecordingCountdown*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RecordingCountdown*>(create->lpCreateParams);
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

LRESULT RecordingCountdown::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        window_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RecordingHud::Create() {
    if (!EnsureGdiplus() || window_ != nullptr) {
        return window_ != nullptr;
    }

    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &RecordingHud::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kHudClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) {
        return false;
    }

    if (!CreateLayeredBitmap(&memory_, &bitmap_, &bits_, width_, height_)) {
        return false;
    }

    screenX_ = GetSystemMetrics(SM_CXSCREEN) - width_ - 24;
    screenY_ = 24;
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kHudClass, L"HaloCap recording", WS_POPUP, screenX_, screenY_,
                              width_, height_, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    Render();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    return true;
}

void RecordingHud::Close() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (bitmap_ != nullptr) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (memory_ != nullptr) {
        DeleteDC(memory_);
        memory_ = nullptr;
    }
    bits_ = nullptr;
}

void RecordingHud::Update(std::int64_t elapsedMs, bool paused) {
    if (window_ == nullptr) {
        return;
    }
    if (elapsedMs == elapsedMs_ && paused == paused_) {
        return;
    }
    elapsedMs_ = elapsedMs;
    paused_ = paused;
    Render();
}

RecordingHud::Action RecordingHud::PollAction() {
    const Action action = pending_;
    pending_ = Action::None;
    return action;
}

void RecordingHud::Render() {
    if (bits_ == nullptr) {
        return;
    }

    pauseRect_ = RECT{ width_ - 108, 10, width_ - 60, height_ - 10 };
    stopRect_ = RECT{ width_ - 52, 10, width_ - 8, height_ - 10 };

    auto* canvas = new Gdiplus::Bitmap(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                                       static_cast<BYTE*>(bits_));
    Gdiplus::Graphics g(canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::GraphicsPath path;
    const float radius = height_ * 0.5f;
    path.AddArc(0.0f, 0.0f, radius * 2.0f, radius * 2.0f, 90.0f, 180.0f);
    path.AddArc(width_ - radius * 2.0f, 0.0f, radius * 2.0f, radius * 2.0f, 270.0f, 180.0f);
    path.CloseFigure();
    Gdiplus::SolidBrush background(Gdiplus::Color(235, 0x16, 0x1A, 0x22));
    g.FillPath(&background, &path);
    Gdiplus::Pen outlinePen(Gdiplus::Color(40, 255, 255, 255), 1.0f);
    g.DrawPath(&outlinePen, &path);

    Gdiplus::SolidBrush dot(paused_ ? Gdiplus::Color(255, 0xF5, 0xB3, 0x3D)
                                    : Gdiplus::Color(255, 0xFF, 0x6B, 0x6B));
    g.FillEllipse(&dot, 18.0f, 20.0f, 12.0f, 12.0f);

    Gdiplus::Font timeFont(L"Consolas", 17.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush timeBrush(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
    const std::wstring time = FormatTime(elapsedMs_);
    Gdiplus::StringFormat left;
    left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(time.c_str(), static_cast<int>(time.size()), &timeFont,
                 Gdiplus::RectF(40.0f, 0.0f, 110.0f, static_cast<float>(height_)), &left,
                 &timeBrush);

    // 暂停/继续按钮：两条竖线或三角形。
    Gdiplus::SolidBrush buttonBrush(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
    if (paused_) {
        Gdiplus::PointF points[3] = { Gdiplus::PointF(width_ - 92.0f, 15.0f),
                                     Gdiplus::PointF(width_ - 78.0f, 26.0f),
                                     Gdiplus::PointF(width_ - 92.0f, 37.0f) };
        g.FillPolygon(&buttonBrush, points, 3);
    } else {
        g.FillRectangle(&buttonBrush, width_ - 92.0f, 15.0f, 5.0f, 22.0f);
        g.FillRectangle(&buttonBrush, width_ - 81.0f, 15.0f, 5.0f, 22.0f);
    }

    // 停止按钮：方块。
    Gdiplus::SolidBrush stopBrush(Gdiplus::Color(255, 0xFF, 0x6B, 0x6B));
    g.FillRectangle(&stopBrush, width_ - 40.0f, 15.0f, 22.0f, 22.0f);

    delete canvas;
    HDC screen = GetDC(nullptr);
    POINT destination{ screenX_, screenY_ };
    SIZE size{ width_, height_ };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(window_, screen, &destination, &size, memory_, &source, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK RecordingHud::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<RecordingHud*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RecordingHud*>(create->lpCreateParams);
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

LRESULT RecordingHud::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (point.x >= pauseRect_.left && point.x < pauseRect_.right &&
            point.y >= pauseRect_.top && point.y < pauseRect_.bottom) {
            pending_ = Action::Pause;
        } else if (point.x >= stopRect_.left && point.x < stopRect_.right &&
                   point.y >= stopRect_.top && point.y < stopRect_.bottom) {
            pending_ = Action::Stop;
        }
        return 0;
    }
    case WM_DESTROY:
        window_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}
