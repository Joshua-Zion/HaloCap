#pragma once

#include "tray_menu.h"

#include <windows.h>

#include <string>
#include <vector>

namespace halocap::app {

// 1.x TrayMenu.xaml 的同款自绘菜单：白色圆角面板、品牌头（光圈标记 + HaloCap/光圈 + 版本号）、
// 彩色图标条目、快捷键列、蓝色勾选、分隔线与「Esc 关闭」页脚。
// 条目内容来自 core::BuildTrayMenu（顺序/可用性/勾选有单测），这一层只管画与交互：
// 鼠标悬停/按下/点击、Up/Down/Home/End/Enter/Esc、点外面收起、关闭后把焦点还给原来的窗口。
class TrayMenuWindow {
public:
    bool Open(const core::TrayMenuState& state, const std::wstring& versionLabel, POINT screen);
    void Close();
    bool Active() const { return window_ != nullptr; }

    // 读取并清空选中的命令（None = 没有选择，只是收起）。
    core::TrayCommand PollCommand();

    void* HandleForTest() const { return window_; }
    int ItemCountForTest() const { return static_cast<int>(items_.size()); }
    // 客户区坐标（含阴影留白）。分隔线返回空矩形。
    RECT ItemRectForTest(int index) const;
    int HighlightForTest() const { return highlight_; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Layout();
    void Render();
    void Present(BYTE alpha = 255);
    void Activate(int index);
    void StepSelection(int direction);
    void SelectEdge(bool first);
    int HitItem(POINT client) const;

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0f;
    int padPx_ = 0;
    HWND previousForeground_ = nullptr;

    std::vector<core::TrayMenuItem> items_;
    std::wstring version_;
    std::vector<RECT> itemRects_;
    float panelHeightDip_ = 0.0f;
    bool recording_ = false;
    bool paused_ = false;
    int highlight_ = -1;
    int pressed_ = -1;
    bool trackingMouse_ = false;
    core::TrayCommand pending_ = core::TrayCommand::None;
};

}