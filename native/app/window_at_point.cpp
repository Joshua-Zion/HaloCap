#include "window_at_point.h"

#include <dwmapi.h>

#include <cwchar>
#include <iterator>
#include <string>

namespace halocap::app {

namespace {

// 这些类名是 1.x WindowProbe 里排除过的同一批：桌面/Shell、工具提示、阴影、输入法。
bool IsSystemDecoration(const wchar_t* className) {
    static const wchar_t* const kExcluded[] = {
        L"Progman",          L"WorkerW",        L"tooltips_class32",
        L"SysShadow",        L"IME",            L"MSCTFIME UI",
    };
    for (const wchar_t* excluded : kExcluded) {
        if (std::wcscmp(className, excluded) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool WindowVisibleRect(HWND window, RECT* rect) {
    if (window == nullptr || rect == nullptr) {
        return false;
    }
    // Win10 之后 GetWindowRect 含一圈不可见的缩放边框，DWM 的才和看到的对齐。
    constexpr DWORD kExtendedFrameBounds = 9;
    if (SUCCEEDED(DwmGetWindowAttribute(window, kExtendedFrameBounds, rect, sizeof(RECT))) &&
        rect->right > rect->left && rect->bottom > rect->top) {
        return true;
    }
    return GetWindowRect(window, rect) != FALSE;
}

HWND WindowAtPoint(POINT screen, HWND ignore, int minSize) {
    if (ignore != nullptr) {
        // 全屏 topmost 的覆盖层不临时禁用就会永远命中它自己。
        EnableWindow(ignore, FALSE);
    }
    HWND target = WindowFromPoint(screen);
    if (ignore != nullptr) {
        EnableWindow(ignore, TRUE);
    }
    if (target == nullptr) {
        return nullptr;
    }

    // WindowFromPoint 常给到浏览器这类应用的子窗口；选区要的是顶层窗口。
    if (const HWND root = GetAncestor(target, GA_ROOT); root != nullptr) {
        target = root;
    }
    if (target == GetShellWindow() || target == GetDesktopWindow()) {
        return nullptr;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(target, &processId);
    if (processId == GetCurrentProcessId() || !IsWindowVisible(target)) {
        return nullptr;
    }

    constexpr DWORD kCloaked = 14;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(target, kCloaked, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return nullptr;
    }
    if ((GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) {
        return nullptr;
    }

    wchar_t className[64] = {};
    GetClassNameW(target, className, static_cast<int>(std::size(className)));
    if (IsSystemDecoration(className)) {
        return nullptr;
    }

    RECT rect{};
    if (!WindowVisibleRect(target, &rect)) {
        return nullptr;
    }
    if (rect.right - rect.left < minSize || rect.bottom - rect.top < minSize) {
        return nullptr;
    }
    return target;
}

}
