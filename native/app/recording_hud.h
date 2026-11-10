#pragma once

#include "geometry.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace halocap::app {

// 录制边框：倒计时开始前就挂在选区/目标窗口上，录制中一直可见，点击穿透。
// 与 1.x 的 RecordingFrameWindow 同一套参数：外圈 2px #FF7A7A、内圈 1px 半透明黑，
// 160ms 淡入，WDA_EXCLUDEFROMCAPTURE（不会录进自己）。
class RecordingFrame {
public:
    bool Show(const core::IntRect& screenRect);
    void UpdateRect(const core::IntRect& screenRect);
    void Close();
    bool Valid() const { return window_ != nullptr; }

    void* HandleForTest() const { return window_; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureSurface(int width, int height);
    void ReleaseSurface();
    void Render();
    void Present(BYTE alpha);

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0f;
    core::IntRect rect_{};
};

// 录制倒计时：选区中央的白色圆角卡片（1.x RecordingCountdownWindow），3/2/1 每秒一跳，
// 数字带 220ms 缩放 + 160ms 淡入的 tick 动画。Esc 取消。模态但泵消息，UI 不假死。
// probe 只给自检用：首帧画好后回调一次（传窗口句柄），不影响正常录制路径。
class RecordingCountdown {
public:
    bool Show(int seconds, const core::IntRect& anchorScreen,
              const std::function<void(HWND)>& probe = {});

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Render(int secondsLeft, float numberScale, float numberAlpha);
    void Present();

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0f;
    int screenX_ = 0;
    int screenY_ = 0;
};

// 录制浮层一次刷新需要的状态，与 1.x 的 RecordingHudState 同源。
struct RecordingHudState {
    std::int64_t elapsedMs = 0;
    bool paused = false;
    std::wstring info;
    bool showSystemLevel = false;
    float systemLevel = 0.0f;
    bool showMicLevel = false;
    float micLevel = 0.0f;
};

// 录制浮层：贴在录制区域上方的白色卡片（1.x RecordingHudWindow）——呼吸红点、REC/PAUSE、
// 时长、信息行（尺寸·帧率·文件大小·音轨）、两路电平表、暂停/停止按钮。
// 位置规则与 1.x 一致：优先区域上方 10px，放不下翻到下方，再夹进工作区。
class RecordingHud {
public:
    enum class Action {
        None,
        Pause,
        Stop,
    };

    bool Create(const core::IntRect& anchorScreen);
    void Close();
    bool Valid() const { return window_ != nullptr; }

    void UpdateAnchor(const core::IntRect& anchorScreen);
    void Update(const RecordingHudState& state);

    // 读取并清空点击动作。
    Action PollAction();

    void* HandleForTest() const { return window_; }
    int PadForTest() const { return padPx_; }
    RECT PauseRectForTest() const { return pauseRect_; }
    RECT StopRectForTest() const { return stopRect_; }

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void PlaceWindow();
    void Render();
    void Present(BYTE alpha);
    int HitButton(POINT client) const;

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int padPx_ = 0;
    float scale_ = 1.0f;
    int screenX_ = 0;
    int screenY_ = 0;
    core::IntRect anchor_{};

    RecordingHudState state_;
    float displaySystemLevel_ = 0.0f;
    float displayMicLevel_ = 0.0f;
    ULONGLONG pulseStart_ = 0;
    bool hasRenderedState_ = false;

    int hovered_ = 0;   // 0 无 / 1 暂停 / 2 停止
    int pressed_ = 0;
    bool buttonsDisabled_ = false;
    bool trackingMouse_ = false;
    Action pending_ = Action::None;
    RECT pauseRect_{};
    RECT stopRect_{};
};

}