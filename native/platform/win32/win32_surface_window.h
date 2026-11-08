#pragma once

#include "surface_window.h"

#include <windows.h>

#include <functional>

namespace halocap::platform::win32 {

// ISurfaceWindow 的 Win32 实现。三条都不是随手写的：
//   1. 透明来自 DirectComposition（实测：HWND 上的 flip 交换链不接受 premultiplied alpha），
//      所以窗口用 WS_EX_NOREDIRECTIONBITMAP 而不是 WS_EX_LAYERED；
//   2. 注册窗口类时 hbrBackground 必须为空，否则显示的一瞬间会闪一块不透明底色；
//   3. 显示只用 SW_SHOWNOACTIVATE，任何路径都不许抢焦点。
class Win32SurfaceWindow final : public core::ISurfaceWindow {
public:
    Win32SurfaceWindow() = default;

    ~Win32SurfaceWindow() override;

    bool Create(const core::WindowBounds& bounds, const core::WindowOptions& options) override;

    bool SetBounds(const core::WindowBounds& bounds) override;

    void SetVisible(bool visible) override;

    void SetClickThrough(bool clickThrough) override;

    core::WindowBounds Bounds() const override { return bounds_; }

    core::NativeHandle Handle() const override { return hwnd_; }

    // 验证用：把实际生效的扩展样式读回来，别只相信传参。
    LONG_PTR EffectiveExStyle() const;

    bool IsVisible() const;

    // The screenshot overlay handles its own mouse and toolbar messages, so it needs
    // a seam into the window procedure. Set it right after Create, before any input.
    using MessageHandler = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;
    void SetMessageHandler(MessageHandler handler);

private:
    static ATOM RegisterClass();

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    MessageHandler handler_;

    HWND hwnd_ = nullptr;
    core::WindowBounds bounds_{};
};

}
