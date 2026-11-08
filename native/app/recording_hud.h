#pragma once

#include <cstdint>
#include <windows.h>

namespace halocap::app {

// 录制倒计时：居中 3/2/1，Esc 取消。模态但会泵消息，所以 UI 不假死。
class RecordingCountdown {
public:
    bool Show(int seconds);

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Render(int secondsLeft);

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

// 录制 HUD：右上角胶囊，红点 + 时长 + 暂停/停止两个按钮。
class RecordingHud {
public:
    enum class Action {
        None,
        Pause,
        Stop,
    };

    bool Create();
    void Close();
    bool Valid() const { return window_ != nullptr; }

    void Update(std::int64_t elapsedMs, bool paused);

    // 读取并清空点击动作。
    Action PollAction();

private:
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Render();
    void Show();

    HWND window_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 280;
    int height_ = 52;
    int screenX_ = 0;
    int screenY_ = 0;
    std::int64_t elapsedMs_ = 0;
    bool paused_ = false;
    Action pending_ = Action::None;
    RECT pauseRect_{};
    RECT stopRect_{};
};

}
