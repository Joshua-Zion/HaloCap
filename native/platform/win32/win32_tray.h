#pragma once

#include "tray_menu.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace halocap::platform::win32 {

// 托盘图标的 Win32 实现。
//
// 菜单的「内容」在 core::BuildTrayMenu（顺序、可用性、勾选、快捷键列，有单测）；
// 菜单的「样子」在 app 层的 TrayMenuWindow——与 1.x 的自绘 WPF ContextMenu 同款。
// 这里保留 DescribeMenu/BuildMenuHandle 只给探针读回模型，证明平台侧没有漏项。
//
// 图标仍然是「光圈」：32×32、四段彩色弧、录制时中心变红点，与 1.x 的
// CreateTrayIcon 同一套参数（颜色取了同一个色板）。
class Win32Tray final {
public:
    enum class Click {
        None,
        Left,
        Right,
    };

    // 只给探针：把菜单模型建成真的 HMENU 再逐项读回来，证明平台侧没有漏项。
    struct MenuSnapshot {
        std::vector<std::int32_t> commands;
        std::vector<std::string> labels;
        std::vector<bool> enabled;
        std::vector<bool> checked;
        std::vector<bool> separators;
    };

    Win32Tray();
    ~Win32Tray();

    Win32Tray(const Win32Tray&) = delete;
    Win32Tray& operator=(const Win32Tray&) = delete;

    // 建隐藏窗口并加图标。没有资源管理器（Shell_NotifyIcon 失败）时返回 false。
    bool Start();
    void Close();

    bool Visible() const { return added_; }

    // 图标（空闲/录制）与提示文字一起更新。
    bool Apply(const core::TrayMenuState& state);

    // 气泡通知。Windows 的通知开关关着时 Shell 仍然接受请求，所以返回值只代表调用成功。
    bool Notify(std::string_view title, std::string_view message);

    // 托盘图标被点击；由消息循环取走。
    Click PollClick();

    // 菜单命令经由 WM_COMMAND 回来的那一半（自绘菜单不走这里，测试与将来的键盘路径走）。
    core::TrayCommand PollCommand();

    // 用户点了「气泡通知」：由消息循环取走，用来跳转到通知对应的位置。
    bool PollBalloonClick();

    // 只给探针与消息循环：抽干本线程的消息。
    void PumpMessages();

    bool DescribeMenu(const core::TrayMenuState& state, MenuSnapshot& out);

    // 只给探针：自己给自己投一条托盘点击消息，验证 WndProc 的消息路径。
    void PostFakeClick(bool rightButton);

    void* Window() const { return window_; }
    const char* TooltipText() const { return tooltip_.c_str(); }
    const char* LastErrorKind() const { return errorKind_; }
    std::int32_t LastErrorCode() const { return lastError_; }

private:
    static bool EnsureClassRegistered();
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool AddIcon();
    void RemoveIcon();
    bool ModifyIcon();
    void* BuildMenuHandle(const core::TrayMenuState& state);
    void SetError(const char* kind, std::int32_t code);

    void* window_ = nullptr;
    void* iconIdle_ = nullptr;
    void* iconRecording_ = nullptr;
    void* currentIcon_ = nullptr;
    bool added_ = false;
    bool recording_ = false;
    std::uint32_t taskbarCreatedMessage_ = 0;

    Click pendingClick_ = Click::None;
    core::TrayCommand pendingCommand_ = core::TrayCommand::None;
    bool pendingBalloonClick_ = false;
    std::string tooltip_;

    const char* errorKind_ = "none";
    std::int32_t lastError_ = 0;
};

}
