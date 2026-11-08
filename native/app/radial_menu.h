#pragma once

#include "tray_menu.h"

#include <functional>
#include <windows.h>

namespace halocap::app {

// 1.x 的圆盘菜单：Ctrl+Alt+D 或托盘左键在光标处弹出一个圆形菜单，
// 四个动作从 12 点开始顺时针（截图 / OCR / 录制 / 贴图）。
//
// 用 GDI+ + 分层窗口画：圆盘本身是低频界面，GDI+ 的抗锯齿文字/圆比
// 再挂一套 D2D 合成链便宜；覆盖层那套高频渲染不适用于这里。
class RadialMenu {
public:
    RadialMenu();
    ~RadialMenu();

    RadialMenu(const RadialMenu&) = delete;
    RadialMenu& operator=(const RadialMenu&) = delete;

    using SelectHandler = std::function<void(core::AppAction)>;

    // 在光标处打开。成功返回 true，动作由 onSelect 回调。
    bool Open(SelectHandler onSelect);
    void Close();
    bool Active() const { return window_ != nullptr; }

private:
    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyHook(int code, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void Render();
    int HitTest(POINT client) const;

    void* window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    POINT center_{};          // 客户区坐标
    int hovered_ = -1;
    SelectHandler onSelect_;
    HHOOK hook_ = nullptr;
};

}
