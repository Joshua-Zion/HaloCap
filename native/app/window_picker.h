#pragma once

#include <windows.h>

#include <functional>

namespace halocap::app {

// 窗口录制的前置拾取：全屏压暗，光标下的窗口高亮描边并显示标题，
// 左键确认、Esc/右键取消。命中测试时临时禁用自身，避免拾取层挡在
// WindowFromPoint 前面。
class WindowPicker {
public:
    WindowPicker();
    ~WindowPicker();

    WindowPicker(const WindowPicker&) = delete;
    WindowPicker& operator=(const WindowPicker&) = delete;

    using PickHandler = std::function<void(HWND)>;
    bool Open(PickHandler onPick);
    void Close();
    bool Active() const { return window_ != nullptr; }

private:
    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyHook(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HWND TargetAt(POINT screen) const;
    void Render();

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    HWND target_ = nullptr;
    POINT cursor_{};
    PickHandler onPick_;
    HHOOK hook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
};

}
