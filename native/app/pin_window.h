#pragma once

#include <cstdint>
#include <vector>
#include <windows.h>

#include <functional>

namespace halocap::app {

// 贴图窗口：无边框、置顶、分层窗口（UpdateLayeredWindow），内容是合成后的截图 + 一圈光晕。
// 拖动移动、双击关闭；不抢焦点（WS_EX_NOACTIVATE），不会打断用户正在做的事。
//
// 1.x 的贴图是 WPF 窗口（带边框、阴影与关闭按钮），这里用分层窗口做同样的视觉语言：
// 白边 + 向外渐隐的深色光晕，拖动/关闭手感一致；悬停关闭按钮等细节留给 UI 那一轮。
class PinWindow {
public:
    PinWindow();
    ~PinWindow();

    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    // bgra: width*height*4，函数返回后调用方可以释放。screenX/Y 是图像左上角。
    bool Create(const std::uint8_t* bgra, int width, int height, int screenX, int screenY);

    // 贴图上的 OCR：把当前图像交给调用方识别。
    using OcrHandler = std::function<void(const std::uint8_t*, int, int)>;
    void SetOcrHandler(OcrHandler handler) { onOcr_ = std::move(handler); }

    void Close();
    bool Valid() const { return window_ != nullptr; }
    HWND Handle() const { return static_cast<HWND>(window_); }

private:
    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void* window_ = nullptr;
    int contentWidth_ = 0;
    int contentHeight_ = 0;

    void Render();
    int ButtonAt(POINT client) const;   // 0 复制 / 1 保存 / 2 关闭 / -1 无

    std::vector<std::uint8_t> content_;
    bool hover_ = false;
    float zoom_ = 1.0f;
    float opacity_ = 1.0f;
    bool trackingLeave_ = false;
    RECT copyRect_{};
    RECT saveRect_{};
    RECT closeRect_{};
    RECT ocrRect_{};
    OcrHandler onOcr_;

    bool dragging_ = false;
    POINT dragOrigin_{};
    POINT dragWindow_{};
};

}
