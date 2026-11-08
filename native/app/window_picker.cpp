#include "window_picker.h"

#include <windowsx.h>

#include <objbase.h>
#include <gdiplus.h>

#include <cstring>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapWindowPicker";
constexpr UINT kMsgEscape = WM_APP + 3;

WindowPicker* g_active = nullptr;

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

std::wstring WindowTitle(HWND window) {
    wchar_t buffer[256] = {};
    GetWindowTextW(window, buffer, 256);
    return buffer;
}

bool IsDesktopWindow(HWND window) {
    return window == GetShellWindow() || window == GetDesktopWindow();
}

}  // namespace

WindowPicker::WindowPicker() = default;
WindowPicker::~WindowPicker() { Close(); }

bool WindowPicker::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &WindowPicker::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hCursor = LoadCursorW(nullptr, IDC_HAND);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool WindowPicker::Open(PickHandler onPick) {
    Close();
    if (!EnsureClass() || !EnsureGdiplus()) {
        return false;
    }
    onPick_ = std::move(onPick);

    width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (width_ < 200 || height_ < 200) {
        width_ = GetSystemMetrics(SM_CXSCREEN);
        height_ = GetSystemMetrics(SM_CYSCREEN);
    }

    HDC screen = GetDC(nullptr);
    memory_ = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap_ == nullptr || bits_ == nullptr) {
        Close();
        return false;
    }
    SelectObject(memory_, bitmap_);
    std::memset(bits_, 0, static_cast<std::size_t>(width_) * height_ * 4);

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                  WS_EX_TRANSPARENT,
                              kClassName, L"HaloCap window picker", WS_POPUP, originX, originY,
                              width_, height_, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        Close();
        return false;
    }

    GetCursorPos(&cursor_);
    target_ = TargetAt(cursor_);
    Render();
    ShowWindow(window_, SW_SHOWNOACTIVATE);

    g_active = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &WindowPicker::KeyHook, GetModuleHandleW(nullptr),
                              0);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &WindowPicker::MouseHook,
                                   GetModuleHandleW(nullptr), 0);
    return true;
}

void WindowPicker::Close() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (g_active == this) {
        g_active = nullptr;
    }
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
    target_ = nullptr;
}

HWND WindowPicker::TargetAt(POINT screen) const {
    HWND window = reinterpret_cast<HWND>(const_cast<WindowPicker*>(this)->window_);
    if (window != nullptr) {
        // 拾取层自己全屏且 topmost，不临时禁用就会永远命中它。
        EnableWindow(window, FALSE);
        HWND target = WindowFromPoint(screen);
        EnableWindow(window, TRUE);
        // WindowFromPoint 常常给到 Chrome 这类应用的子窗口；录制要顶层窗口。
        if (target != nullptr) {
            const HWND root = GetAncestor(target, GA_ROOT);
            if (root != nullptr) {
                target = root;
            }
        }
        if (target != nullptr) {
            DWORD processId = 0;
            GetWindowThreadProcessId(target, &processId);
            RECT rect{};
            GetWindowRect(target, &rect);
            if (processId != GetCurrentProcessId() && IsWindowVisible(target) &&
                !IsDesktopWindow(target) && rect.right - rect.left > 80 &&
                rect.bottom - rect.top > 60) {
                return target;
            }
        }
    }
    return nullptr;
}

void WindowPicker::Render() {
    if (bits_ == nullptr) {
        return;
    }

    Gdiplus::Bitmap canvas(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                           static_cast<BYTE*>(bits_));
    Gdiplus::Graphics graphics(&canvas);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::SolidBrush dim(Gdiplus::Color(58, 0, 0, 0));
    graphics.FillRectangle(&dim, 0, 0, width_, height_);

    if (target_ != nullptr) {
        RECT targetRect{};
        GetWindowRect(target_, &targetRect);
        const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const Gdiplus::Rect rect(targetRect.left - originX, targetRect.top - originY,
                                 targetRect.right - targetRect.left,
                                 targetRect.bottom - targetRect.top);

        Gdiplus::SolidBrush highlight(Gdiplus::Color(36, 0x2F, 0x7C, 0xF6));
        graphics.FillRectangle(&highlight, rect);
        Gdiplus::Pen border(Gdiplus::Color(255, 0x2F, 0x7C, 0xF6), 2.5f);
        graphics.DrawRectangle(&border, rect);

        const std::wstring title = WindowTitle(target_);
        if (!title.empty()) {
            Gdiplus::Font font(L"Microsoft YaHei UI", 12.0f, Gdiplus::FontStyleRegular,
                               Gdiplus::UnitPixel);
            Gdiplus::StringFormat format;
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const int chipX = std::max(8, rect.X);
            const int chipY = std::max(8, rect.Y - 30);
            Gdiplus::SolidBrush chip(Gdiplus::Color(235, 0x16, 0x1A, 0x22));
            graphics.FillRectangle(&chip, chipX, chipY, 260, 24);
            Gdiplus::SolidBrush text(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
            graphics.DrawString(title.c_str(), static_cast<int>(title.size()), &font,
                                Gdiplus::RectF(static_cast<float>(chipX + 8),
                                               static_cast<float>(chipY), 244.0f, 24.0f),
                                &format, &text);
        }
    } else {
        Gdiplus::Font font(L"Microsoft YaHei UI", 14.0f, Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush text(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
        graphics.DrawString(L"把光标移到要录制的窗口上，左键确认 · Esc 取消", 26, &font,
                            Gdiplus::RectF(0.0f, static_cast<float>(height_) * 0.5f - 20.0f,
                                           static_cast<float>(width_), 40.0f),
                            &format, &text);
    }

    HDC screen = GetDC(nullptr);
    POINT destination{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
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

LRESULT CALLBACK WindowPicker::KeyHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_active == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
        reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode == VK_ESCAPE) {
        PostMessageW(g_active->window_, kMsgEscape, 0, 0);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK WindowPicker::MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_active == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_MOUSEMOVE) {
        const HWND target = g_active->TargetAt(mouse->pt);
        if (target != g_active->target_) {
            g_active->target_ = target;
            g_active->Render();
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if (wParam == WM_LBUTTONDOWN) {
        const HWND target = g_active->target_;
        if (target != nullptr) {
            PostMessageW(g_active->window_, WM_APP + 4, reinterpret_cast<WPARAM>(target), 0);
        } else {
            PostMessageW(g_active->window_, kMsgEscape, 0, 0);
        }
        return 1;
    }
    if (wParam == WM_RBUTTONDOWN) {
        PostMessageW(g_active->window_, kMsgEscape, 0, 0);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK WindowPicker::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<WindowPicker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<WindowPicker*>(create->lpCreateParams);
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

LRESULT WindowPicker::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_MOUSEMOVE) {
        POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(window, &screen);
        cursor_ = screen;
        const HWND target = TargetAt(screen);
        if (target != target_) {
            target_ = target;
            Render();
        }
        return 0;
    }
    if (message == WM_APP + 4) {
        const HWND target = reinterpret_cast<HWND>(wParam);
        const PickHandler handler = onPick_;
        Close();
        if (handler && target != nullptr) {
            handler(target);
        }
        return 0;
    }
    if (message == kMsgEscape) {
        Close();
        return 0;
    }
    if (message == WM_DESTROY) {
        window_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}
