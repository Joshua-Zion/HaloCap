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

// 贴图窗口是用户内容，1.x 允许把它当截图目标；其余本进程窗口（覆盖层、拾取器、
// 圆盘、录制框）一律跳过，否则全屏 topmost 的覆盖层永远命中自己。
constexpr wchar_t kPinWindowTitle[] = L"HaloCap pin";

bool IsOwnProcessWindow(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) {
        return false;
    }
    wchar_t title[64] = {};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    return std::wcscmp(title, kPinWindowTitle) != 0;
}

bool IsCloaked(HWND window) {
    constexpr DWORD kCloaked = 14;
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, kCloaked, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

struct ProbeContext {
    POINT point;
    HWND ignore;
    int minSize;
    HWND found;
    RECT foundRect;
};

BOOL CALLBACK ProbeWindow(HWND window, LPARAM param) {
    auto* context = reinterpret_cast<ProbeContext*>(param);
    if (context->found != nullptr) {
        return FALSE;
    }
    if (window == context->ignore || IsOwnProcessWindow(window) || !IsWindowVisible(window)) {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TRANSPARENT) != 0) {
        return TRUE;
    }

    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (IsSystemDecoration(className)) {
        return TRUE;
    }

    RECT rect{};
    if (!WindowVisibleRect(window, &rect)) {
        return TRUE;
    }
    if (rect.right - rect.left < context->minSize || rect.bottom - rect.top < context->minSize) {
        return TRUE;
    }
    if (context->point.x < rect.left || context->point.x >= rect.right ||
        context->point.y < rect.top || context->point.y >= rect.bottom) {
        return TRUE;
    }
    if (IsCloaked(window)) {
        return TRUE;
    }

    context->found = window;
    context->foundRect = rect;
    return FALSE;
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
    // 1.x 的 WindowProbe 用 Z 序枚举而不是 WindowFromPoint：遮罩在最上层，
    // WindowFromPoint 拿到的永远是它自己（临时 EnableWindow(FALSE) 这种做法会
    // 触碰窗口状态，实测在 DComp 覆盖层上也不可靠）。EnumWindows 本身就是
    // topmost → 普通窗口的 Z 序，第一个命中的就是光标下最靠前的窗口。
    ProbeContext context{ screen, ignore, minSize > 0 ? minSize : 32, nullptr, RECT{} };
    EnumWindows(&ProbeWindow, reinterpret_cast<LPARAM>(&context));
    return context.found;
}

}
