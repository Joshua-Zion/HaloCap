// 托盘菜单的 pure 部分：菜单项、可用性、勾选、快捷键列与提示文字。
// 这些规则全是从 1.x 的 TrayMenu.BuildMenu 与 TrayIconService 抄下来的，现在能离线钉住。
#include "tray_menu.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace core = halocap::core;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s: expected true\n", what);
    }
}

void CheckFalse(bool actual, const char* what) {
    ++checks;
    if (actual) {
        ++failures;
        std::printf("FAIL %s: expected false\n", what);
    }
}

void CheckText(const std::string& actual, const std::string& expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, actual.c_str(), expected.c_str());
    }
}

void CheckItem(const core::TrayMenuItem& item, core::TrayCommand command, const char* label,
               const char* what) {
    ++checks;
    if (item.separator || item.command != command || item.label != label) {
        ++failures;
        std::printf("FAIL %s: got separator=%d command=%d label=\"%s\"\n", what, item.separator ? 1 : 0,
                    static_cast<int>(item.command), item.label.c_str());
    }
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

void TestActionTable() {
    CheckText(core::AppActionTitle(core::AppAction::Screenshot), "截图", "screenshot title");
    CheckText(core::AppActionTitle(core::AppAction::Ocr), "文字识别", "ocr title");
    CheckText(core::AppActionTitle(core::AppAction::Record), "录屏", "record title");
    CheckText(core::AppActionTitle(core::AppAction::Pin), "贴图", "pin title");

    core::AppAction action = core::AppAction::Screenshot;
    CheckTrue(core::AppActionFromIndex(0, action), "index 0 is an action");
    CheckTrue(action == core::AppAction::Screenshot, "index 0 is screenshot");
    CheckTrue(core::AppActionFromIndex(3, action), "index 3 is an action");
    CheckTrue(action == core::AppAction::Pin, "index 3 is pin");
    CheckFalse(core::AppActionFromIndex(-1, action), "index -1 is not an action");
    CheckFalse(core::AppActionFromIndex(4, action), "index 4 is not an action");

    CheckFalse(core::IsTrayCommand(0), "0 is not a command");
    CheckFalse(core::IsTrayCommand(1000), "1000 is not a command");
    CheckTrue(core::IsTrayCommand(1001), "the first command is in range");
    CheckTrue(core::IsTrayCommand(1009), "the last command is in range");
    CheckFalse(core::IsTrayCommand(1010), "1010 is out of range");
}

void TestIdleMenu() {
    const core::TrayMenuState state = IdleState();
    const std::vector<core::TrayMenuItem> items = core::BuildTrayMenu(state);

    CheckEqual(static_cast<long long>(items.size()), 11, "idle menu has eleven rows");

    CheckItem(items[0], core::TrayCommand::Screenshot, "截图", "row 0 is screenshot");
    CheckText(items[0].shortcut, "Ctrl+Alt+A", "screenshot shortcut");
    CheckTrue(items[0].enabled, "screenshot is enabled while idle");

    CheckItem(items[1], core::TrayCommand::Ocr, "文字识别", "row 1 is ocr");
    CheckText(items[1].shortcut, "Ctrl+Alt+O", "ocr shortcut");

    CheckItem(items[2], core::TrayCommand::Record, "录屏", "row 2 is record");
    CheckText(items[2].shortcut, "Ctrl+Alt+R", "record shortcut");

    CheckItem(items[3], core::TrayCommand::Pin, "贴图", "row 3 is pin");
    CheckText(items[3].shortcut, "Ctrl+Alt+P", "pin shortcut");
    CheckTrue(items[3].enabled, "pin is enabled when the clipboard has content");

    CheckTrue(items[4].separator, "row 4 is a separator");

    CheckItem(items[5], core::TrayCommand::OpenRadial, "打开光圈", "row 5 opens the radial menu");
    CheckText(items[5].shortcut, "Ctrl+Alt+D", "radial shortcut");
    CheckTrue(items[5].enabled, "radial menu is enabled while idle");

    CheckItem(items[6], core::TrayCommand::Settings, "设置…", "row 6 is settings");
    CheckText(items[6].shortcut, "", "settings has no shortcut");
    CheckTrue(items[6].enabled, "settings is enabled while idle");

    CheckTrue(items[7].separator, "row 7 is a separator");

    CheckItem(items[8], core::TrayCommand::ToggleAutoStart, "开机自启", "row 8 is autostart");
    CheckTrue(items[8].checked, "autostart is checked when enabled");
    CheckTrue(items[8].enabled, "autostart is enabled while idle");

    CheckTrue(items[9].separator, "row 9 is a separator");

    CheckItem(items[10], core::TrayCommand::Exit, "退出 HaloCap", "row 10 exits");
    CheckTrue(items[10].enabled, "exit is always enabled");

    // 没有可用剪贴板内容时只灰掉贴图，其余不动。
    core::TrayMenuState noPin = state;
    noPin.pinAvailable = false;
    const std::vector<core::TrayMenuItem> noPinItems = core::BuildTrayMenu(noPin);
    CheckEqual(static_cast<long long>(noPinItems.size()), 11, "menu with no clipboard content has the same rows");
    CheckFalse(noPinItems[3].enabled, "pin is disabled without clipboard content");
    CheckTrue(noPinItems[0].enabled, "screenshot stays enabled without clipboard content");

    // 自启没开时只是不勾选。
    core::TrayMenuState noAutoStart = state;
    noAutoStart.autoStart = false;
    CheckFalse(core::BuildTrayMenu(noAutoStart)[8].checked, "autostart is unchecked when disabled");
}

void TestRecordingMenu() {
    core::TrayMenuState state = IdleState();
    state.recording = true;

    const std::vector<core::TrayMenuItem> items = core::BuildTrayMenu(state);
    CheckEqual(static_cast<long long>(items.size()), 12, "recording menu has twelve rows");

    // 录制中只有「停止录制」与「暂停录制」是亮的，其余全灰——1.x 的原规则。
    CheckFalse(items[0].enabled, "screenshot is disabled while recording");
    CheckFalse(items[1].enabled, "ocr is disabled while recording");
    CheckItem(items[2], core::TrayCommand::Record, "停止录制", "record becomes stop while recording");
    CheckTrue(items[2].enabled, "stop recording stays enabled");
    CheckText(items[2].shortcut, "Ctrl+Alt+R", "stop keeps the record shortcut");

    CheckItem(items[3], core::TrayCommand::PauseRecording, "暂停录制", "pause follows stop");
    CheckText(items[3].shortcut, "", "pause has no shortcut");
    CheckTrue(items[3].enabled, "pause is enabled while recording");

    CheckItem(items[4], core::TrayCommand::Pin, "贴图", "pin follows pause");
    CheckFalse(items[4].enabled, "pin is disabled while recording");

    CheckTrue(items[5].separator, "recording menu separator after the actions");
    CheckFalse(items[6].enabled, "radial menu is disabled while recording");
    CheckFalse(items[7].enabled, "settings is disabled while recording");

    CheckTrue(items[8].separator, "recording menu separator before autostart");
    CheckFalse(items[9].enabled, "autostart is disabled while recording");
    CheckTrue(items[11].enabled, "exit stays enabled while recording");

    // 暂停之后同一项变成「继续录制」，编号不变。
    core::TrayMenuState paused = state;
    paused.recordingPaused = true;
    const std::vector<core::TrayMenuItem> pausedItems = core::BuildTrayMenu(paused);
    CheckItem(pausedItems[3], core::TrayCommand::PauseRecording, "继续录制", "pause becomes resume");
    CheckEqual(static_cast<long long>(pausedItems.size()), 12, "paused menu keeps its rows");
}

void TestTooltip() {
    core::TrayMenuState idle = IdleState();
    CheckText(core::TrayTooltip(idle), "HaloCap · Ctrl+Alt+D", "idle tooltip shows the hotkey");

    core::TrayMenuState noHotkey;
    CheckText(core::TrayTooltip(noHotkey), "HaloCap 光圈", "no hotkey falls back to the product name");

    core::TrayMenuState recording = IdleState();
    recording.recording = true;
    CheckText(core::TrayTooltip(recording), "HaloCap · 正在录制", "recording tooltip");

    recording.recordingPaused = true;
    CheckText(core::TrayTooltip(recording), "HaloCap · 录制已暂停", "paused tooltip wins over recording");
}

void TestTruncation() {
    const std::string ascii63(63, 'a');
    CheckText(core::TruncateTooltipUtf16(ascii63, core::kTrayTooltipMaxUnits), ascii63,
              "63 ASCII characters fit exactly");

    const std::string ascii80(80, 'a');
    CheckEqual(static_cast<long long>(core::TruncateTooltipUtf16(ascii80, 63).size()), 63,
               "80 ASCII characters are cut to 63 units");

    // 中文是 BMP：一个码点一个 UTF-16 码元，但占 3 个 UTF-8 字节。
    std::string cjk;
    for (int i = 0; i < 70; ++i) {
        cjk += "光";
    }
    CheckEqual(static_cast<long long>(core::TruncateTooltipUtf16(cjk, 63).size()), 63 * 3,
               "70 CJK characters are cut to 63 units, not bytes");

    // 表情是代理对：一个码点两个 UTF-16 码元，截断不能切开。
    std::string emoji;
    for (int i = 0; i < 32; ++i) {
        emoji += "\xF0\x9F\x98\x80";   // U+1F600
    }
    const std::string cutEmoji = core::TruncateTooltipUtf16(emoji, 63);
    CheckEqual(static_cast<long long>(cutEmoji.size()), 31 * 4, "31 emoji = 62 units fit, the 32nd would be 64");

    // 混排：两个 ASCII + 31 个表情 = 64，超了；只放 30 个表情。
    const std::string mixed = "ab" + emoji;
    const std::string cutMixed = core::TruncateTooltipUtf16(mixed, 63);
    CheckEqual(static_cast<long long>(cutMixed.size()), 2 + 30 * 4, "mixed text stops at 62 units");

    // 非法输入在这里截断，输出始终是合法 UTF-8。
    CheckText(core::TruncateTooltipUtf16("ab\xE4\xB8", 63), "ab", "a truncated sequence is dropped");
    CheckText(core::TruncateTooltipUtf16("ab\xFF", 63), "ab", "an invalid lead byte is dropped");

    CheckText(core::TruncateTooltipUtf16("abc", 0), "", "zero units truncates to nothing");
    CheckText(core::TruncateTooltipUtf16("abc", -5), "", "negative units truncates to nothing");
    CheckText(core::TruncateTooltipUtf16("", 63), "", "empty input stays empty");
}

}

int main() {
    TestActionTable();
    TestIdleMenu();
    TestRecordingMenu();
    TestTooltip();
    TestTruncation();

    if (failures != 0) {
        std::printf("TRAY_MENU_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("TRAY_MENU_TESTS=OK checks=%d\n", checks);
    return 0;
}
