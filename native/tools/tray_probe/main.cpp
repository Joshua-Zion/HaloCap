// 托盘链路的一次跑通：图标加得上、菜单模型在平台侧没有走样、点击与命令两条消息路径
// 都通，最后把图标摘掉。菜单本身不弹出来——探针要能无人值守跑完，所以用
// DescribeMenu 把模型建成真的 HMENU 再逐项读回来。
//
// 会在托盘里短暂出现一个「光圈」图标（跑完即摘）。没有资源管理器时 Start 会失败，
// 这时只跳过图标相关的两条断言，其余照跑。
#include "win32_tray.h"

#include "tray_menu.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace core = halocap::core;
using halocap::platform::win32::Win32Tray;

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckText(const std::string& actual, const std::string& expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, actual.c_str(), expected.c_str());
    }
}

// ---- 环境诊断：托盘加不上时，要能说清是「我们的图标不行」还是「这个进程碰不到 Shell」 ----

void PrintEnvironment() {
    wchar_t station[64] = {};
    wchar_t desktop[64] = {};
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof(station), nullptr);
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop,
                              sizeof(desktop), nullptr);
    std::printf("PROBE_WINDOW_STATION=%ls\nPROBE_DESKTOP=%ls\n", station, desktop);

    DWORD integrity = 0xFFFFFFFF;
    DWORD appContainer = 0xFFFFFFFF;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        std::vector<std::uint8_t> buffer(size);
        if (size > 0 &&
            GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
            PSID sid = label->Label.Sid;
            integrity = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        DWORD returned = 0;
        if (GetTokenInformation(token, TokenIsAppContainer, &appContainer, sizeof(appContainer),
                                &returned)) {
            // 0xFFFFFFFF 表示这个 SDK/系统读不出来（Win7 时代没有这个信息类）。
        } else {
            appContainer = 0xFFFFFFFF;
        }
        CloseHandle(token);
    }
    std::printf("PROBE_INTEGRITY=%lu\n", integrity);
    std::printf("PROBE_APPCONTAINER=%lu\n", appContainer);

    const HWND trayWindow = FindWindowW(L"Shell_TrayWnd", nullptr);
    std::printf("SHELL_TRAY_HWND=%p\n", static_cast<void*>(trayWindow));
}

// 用系统自带图标再加一次：如果这也进不去，问题在「这个进程与 Shell 之间」，
// 不在我们用 CreateIconIndirect 造的那张图标。
bool TryStockIcon(HWND window) {
    if (window == nullptr) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 0xFFFF;
    data.uFlags = NIF_ICON | NIF_TIP;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(data.szTip, L"HaloCap probe", 128);

    const bool added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (added) {
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    return added;
}

core::TrayMenuState IdleState() {
    core::TrayMenuState state;
    state.radialHotkey = "Ctrl+Alt+D";
    state.screenshotHotkey = "Ctrl+Alt+A";
    state.ocrHotkey = "Ctrl+Alt+O";
    state.recordHotkey = "Ctrl+Alt+R";
    state.pinHotkey = "Ctrl+Alt+P";
    state.autoStart = true;
    state.pinAvailable = true;
    return state;
}

// 平台把 label 与 shortcut 用 \t 拼在一条文本里（标准菜单的右对齐写法），
// 这里拆开与 core 的模型逐项对齐。
void CheckMenuMatches(const char* what, const std::vector<core::TrayMenuItem>& expected,
                      const Win32Tray::MenuSnapshot& actual) {
    CheckEqual(static_cast<long long>(actual.commands.size()),
               static_cast<long long>(expected.size()), what);

    if (actual.commands.size() != expected.size()) {
        return;
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i].separator) {
            Check(actual.separators[i], "expected a separator");
            continue;
        }

        Check(!actual.separators[i], "expected a menu item, got a separator");
        CheckEqual(actual.commands[i], static_cast<long long>(expected[i].command),
                   "menu command id");

        const std::string& text = actual.labels[i];
        const std::size_t tab = text.find('\t');
        const std::string label = tab == std::string::npos ? text : text.substr(0, tab);
        const std::string shortcut = tab == std::string::npos ? std::string{} : text.substr(tab + 1);

        CheckText(label, expected[i].label, "menu label");
        CheckText(shortcut, expected[i].shortcut, "menu shortcut column");
        Check(actual.enabled[i] == expected[i].enabled, "menu enabled flag");
        Check(actual.checked[i] == expected[i].checked, "menu checked flag");
    }
}

}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    PrintEnvironment();

    Win32Tray tray;
    const bool started = tray.Start();
    std::printf("TRAY_ADDED=%d\n", started ? 1 : 0);
    if (!started) {
        // 没有资源管理器（或图标已被同一进程加过）时只打原因，其余检查照跑。
        std::printf("TRAY_ADD_BLOCKED=%s code=%d\n", tray.LastErrorKind(),
                    static_cast<int>(tray.LastErrorCode()));
        std::printf("STOCK_ICON_ADDED=%d\n", TryStockIcon(static_cast<HWND>(tray.Window())) ? 1 : 0);
    } else {
        Check(true, "the tray icon is added");
    }

    const core::TrayMenuState idle = IdleState();
    // Apply 的结果只在图标真加上时才算数；但提示文字与录制定位在 ModifyIcon 之前就更新了，
    // 所以下面这两条无论有没有 Shell 都能验。
    const bool appliedIdle = tray.Apply(idle);
    std::printf("TRAY_APPLY_IDLE=%d\n", appliedIdle ? 1 : 0);
    std::printf("TRAY_TOOLTIP=%s\n", tray.TooltipText());
    CheckText(tray.TooltipText(), "HaloCap · Ctrl+Alt+D", "idle tooltip shows the radial hotkey");
    if (started) {
        Check(appliedIdle, "idle state applies");
    }

    Win32Tray::MenuSnapshot idleMenu;
    Check(tray.DescribeMenu(idle, idleMenu), "idle menu can be built and read back");
    std::printf("MENU_IDLE_ROWS=%zu\n", idleMenu.commands.size());
    CheckMenuMatches("idle menu matches the core model", core::BuildTrayMenu(idle), idleMenu);

    core::TrayMenuState recording = idle;
    recording.recording = true;
    Win32Tray::MenuSnapshot recordingMenu;
    Check(tray.DescribeMenu(recording, recordingMenu), "recording menu can be built and read back");
    std::printf("MENU_RECORDING_ROWS=%zu\n", recordingMenu.commands.size());
    CheckMenuMatches("recording menu matches the core model", core::BuildTrayMenu(recording),
                     recordingMenu);

    // 两条消息路径：托盘点击（WndProc 记录）与菜单命令（WM_COMMAND）。
    // 它们只需要窗口，不需要 Shell——图标加不上时照样能验。
    Check(tray.Window() != nullptr, "the tray host window exists after Start");
    if (tray.Window() != nullptr) {
        const bool appliedRecording = tray.Apply(recording);
        std::printf("TRAY_APPLY_RECORDING=%d\n", appliedRecording ? 1 : 0);
        std::printf("TRAY_TOOLTIP_RECORDING=%s\n", tray.TooltipText());
        CheckText(tray.TooltipText(), "HaloCap · 正在录制", "recording tooltip replaces the hotkey");
        if (started) {
            Check(appliedRecording, "recording state applies");
        }

        tray.PostFakeClick(true);
        tray.PumpMessages();
        Check(tray.PollClick() == Win32Tray::Click::Right, "right click is delivered through the window");

        tray.PostFakeClick(false);
        tray.PumpMessages();
        Check(tray.PollClick() == Win32Tray::Click::Left, "left click is delivered through the window");
        Check(tray.PollClick() == Win32Tray::Click::None, "a click is only reported once");

        PostMessageW(static_cast<HWND>(tray.Window()), WM_COMMAND,
                     static_cast<WPARAM>(core::TrayCommand::Record), 0);
        tray.PumpMessages();
        Check(tray.PollCommand() == core::TrayCommand::Record,
              "a menu command round-trips through WM_COMMAND");

        if (started) {
            const bool notified = tray.Notify("HaloCap", "托盘通知测试");
            std::printf("TRAY_NOTIFY=%d\n", notified ? 1 : 0);
            Check(notified, "a balloon notification is accepted by the shell");
        }
    }

    tray.Close();
    std::printf("TRAY_REMOVED=%d\n", tray.Visible() ? 0 : 1);
    Check(!tray.Visible(), "the tray icon is removed on close");

    std::printf("TRAY_PROBE_CHECKS=%d\n", checks);
    if (failures != 0) {
        std::printf("TRAY_PROBE=FAILED %d\n", failures);
        return 1;
    }

    std::printf("TRAY_PROBE=OK\n");
    return 0;
}
