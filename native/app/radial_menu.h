#pragma once

#include "settings.h"
#include "tray_menu.h"

#include <functional>
#include <memory>
#include <windows.h>

namespace Gdiplus {
class Graphics;
}

namespace halocap::app {

struct RadialMenuFrameCache;

// 1.x 的光圈：380×380 设计画布，白色径向盘 + 四个 90° 扇区 + 中心 104 圆盘，
// 四个动作在半径 110 处，悬停时扇区着色、图标外移 4 DIP，中心标题变成动作名。
//
// 交互照 1.x：低级鼠标/键盘钩子驱动（不激活窗口），死区 52 拖中心、扇区按下即选、
// 盘外关闭、1–4 / Esc；窗口大小由菜单尺寸（360/420/480 DIP）决定。
class RadialMenu {
public:
    RadialMenu();
    ~RadialMenu();

    RadialMenu(const RadialMenu&) = delete;
    RadialMenu& operator=(const RadialMenu&) = delete;

    using SelectHandler = std::function<void(core::AppAction)>;

    bool Open(SelectHandler onSelect, core::MenuSize size);
    void Close();
    bool Active() const { return window_ != nullptr; }

    // 自检用：窗口句柄（发 WM_MOUSEMOVE 触发悬停）。
    void* HandleForTest() const { return window_; }

private:
    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyHook(int code, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void Render();
    void Tick();
    void UpdateHover(POINT screenPoint);
    void Select(int index);
    void StartHide(int pendingAction);
    void StartDrag(POINT screenPoint);
    void DragTo(POINT screenPoint);

    void* window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int widthPx_ = 0;
    int heightPx_ = 0;
    int windowDip_ = 380;
    float pixelScale_ = 1.0f;   // 窗口像素 / DIP
    float contentScale_ = 1.0f; // 设计画布 → 内容区
    POINT centerScreen_{};
    RECT workArea_{};
    int hovered_ = -1;
    float hoverAlpha_[4] = {};
    bool animating_ = false;
    bool dragging_ = false;
    POINT dragOffset_{};
    unsigned long long showStartMs_ = 0;
    bool closing_ = false;
    unsigned long long closeStartMs_ = 0;
    int pendingAction_ = -1;
    SelectHandler onSelect_;
    std::unique_ptr<RadialMenuFrameCache> cache_;

    void BuildCache();
    void BuildFrame(int index);
    void DrawDesign(Gdiplus::Graphics& graphics, int hoverIndex);
    HHOOK mouseHook_ = nullptr;
    HHOOK keyHook_ = nullptr;
};

}
