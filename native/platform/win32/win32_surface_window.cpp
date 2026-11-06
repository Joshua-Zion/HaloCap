#include "win32_surface_window.h"

namespace halocap::platform::win32 {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapNativeSurface";

}

ATOM Win32SurfaceWindow::RegisterClass() {
    static const ATOM atom = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.style = CS_HREDRAW | CS_VREDRAW;
        description.lpfnWndProc = &DefWindowProcW;
        description.hInstance = GetModuleHandleW(nullptr);
        // 故意留空：给一块底色刷等于给一次闪白。
        description.hbrBackground = nullptr;
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description);
    }();

    return atom;
}

Win32SurfaceWindow::~Win32SurfaceWindow() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool Win32SurfaceWindow::Create(const core::WindowBounds& bounds, const core::WindowOptions& options) {
    if (hwnd_ != nullptr) {
        return false;
    }

    if (RegisterClass() == 0) {
        return false;
    }

    // 透明来自 DirectComposition，不再需要 WS_EX_LAYERED；
    // NOREDIRECTIONBITMAP 让窗口不建重定向位图，内容由合成器直接拿我们的交换链。
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;
    if (options.clickThrough) {
        exStyle |= WS_EX_TRANSPARENT;
    }
    if (options.topMost) {
        exStyle |= WS_EX_TOPMOST;
    }
    if (!options.focusable) {
        exStyle |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    }

    hwnd_ = CreateWindowExW(exStyle,
                            kClassName,
                            L"HaloCap native surface",
                            WS_POPUP,
                            bounds.x,
                            bounds.y,
                            bounds.width,
                            bounds.height,
                            nullptr,
                            nullptr,
                            GetModuleHandleW(nullptr),
                            nullptr);
    if (hwnd_ == nullptr) {
        return false;
    }

    bounds_ = bounds;
    SetVisible(options.visible);
    return true;
}

bool Win32SurfaceWindow::SetBounds(const core::WindowBounds& bounds) {
    if (hwnd_ == nullptr) {
        return false;
    }

    // 覆盖层永远不因为改尺寸而抢焦点。
    if (SetWindowPos(hwnd_, nullptr, bounds.x, bounds.y, bounds.width, bounds.height,
                     SWP_NOACTIVATE | SWP_NOZORDER) == 0) {
        return false;
    }

    bounds_ = bounds;
    return true;
}

void Win32SurfaceWindow::SetVisible(bool visible) {
    if (hwnd_ == nullptr) {
        return;
    }

    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Win32SurfaceWindow::SetClickThrough(bool clickThrough) {
    if (hwnd_ == nullptr) {
        return;
    }

    const LONG_PTR current = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    LONG_PTR next = current;
    if (clickThrough) {
        next |= WS_EX_TRANSPARENT;
    } else {
        next &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }

    if (next == current) {
        return;
    }

    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, next);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

LONG_PTR Win32SurfaceWindow::EffectiveExStyle() const {
    return hwnd_ == nullptr ? 0 : GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
}

bool Win32SurfaceWindow::IsVisible() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != 0;
}

}
