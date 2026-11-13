#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// 圆盘与托盘共用的一份动作集合：编号与 1.x 的 `RadialAction` 一致（0..3），
// 顺序也是圆盘里从 12 点开始顺时针的那一个顺序。
//
// 为什么现在就把这个枚举定下来：托盘菜单的四项就是这四个动作，圆盘将来也要用同一份。
// 两处各写一份 `Screenshot/Ocr/Record/Pin` 的代价是热键表、设置页和诊断页要各翻译一遍。
enum class AppAction : std::int32_t {
    Screenshot = 0,
    Ocr = 1,
    Record = 2,
    Pin = 3,
};

constexpr std::int32_t kAppActionCount = 4;

// 显示名与 1.x 的 `RadialActionInfo.Title` 同源。
const char* AppActionTitle(AppAction action);

bool AppActionFromIndex(std::int32_t index, AppAction& out);

// 菜单命令。编号从 1001 起：大于 0、与任何 WM_ 常量都不撞，平台可以把它当命令 id
// 原样带回（`TrackPopupMenu` 与 `WM_COMMAND` 两条路都用同一套编号）。
enum class TrayCommand : std::int32_t {
    None = 0,

    Screenshot = 1001,
    Ocr = 1002,
    Record = 1003,
    Pin = 1004,
    PauseRecording = 1005,
    OpenRadial = 1006,
    Settings = 1007,
    ToggleAutoStart = 1008,
    Exit = 1009,
    RestartElevated = 1010,
};

bool IsTrayCommand(std::int32_t id);

// 命令的稳定短名，日志与诊断用（"Screenshot" / "OpenRadial" …）。
const char* TrayCommandName(TrayCommand command);

// 托盘菜单的全部输入。这里没有「版本号」之外的界面细节：菜单项的文字、顺序、
// 可用性与勾选状态都由 BuildTrayMenu 决定，平台只负责把它们画出来。
struct TrayMenuState {
    bool recording = false;
    bool recordingPaused = false;
    bool pinAvailable = true;   // 剪贴板里有没有能贴的内容
    bool autoStart = false;     // 开机自启当前开着没有
    bool elevated = false;      // 当前进程是否已提权（提权后不再显示「以管理员身份重启」）

    // 显示文本（"Ctrl+Alt+D"）。空串表示这一项不显示快捷键那一列。
    std::string radialHotkey;
    std::string screenshotHotkey;
    std::string ocrHotkey;
    std::string recordHotkey;
    std::string pinHotkey;
};

struct TrayMenuItem {
    bool separator = false;
    TrayCommand command = TrayCommand::None;
    std::string label;      // UTF-8
    std::string shortcut;   // UTF-8；空串 = 没有快捷键可显示
    bool enabled = true;
    bool checked = false;
};

// 顺序、可用性与勾选规则逐条对齐 1.x 的 `TrayMenu.BuildMenu`：
// 录制中只有「停止录制 / 暂停录制」可用，截图、OCR、贴图、打开光圈、设置、自启全部灰掉；
// 贴图还要剪贴板里真有内容才可用。
std::vector<TrayMenuItem> BuildTrayMenu(const TrayMenuState& state);

// 托盘提示文字：录制态优先，其次「HaloCap · 热键」，最后是产品名。
std::string TrayTooltip(const TrayMenuState& state);

// Shell 的 `szTip` 现在是 128 个 wchar，但 1.x 按 63 截断（老版本留下的限制），
// 2.0 沿用同一个数。按 UTF-16 码元截断，且不从代理对中间切开——切开的话
// Shell 会显示半个字。这条逻辑留在 core 是为了能用纯文本测到边界。
constexpr int kTrayTooltipMaxUnits = 63;

std::string TruncateTooltipUtf16(std::string_view utf8, int maxUnits);

}
