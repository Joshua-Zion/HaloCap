#include "input_host.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using halocap::core::DecideSwallow;
using halocap::core::FormatHotkeyText;
using halocap::core::HotkeySpec;
using halocap::core::KeyEvent;
using halocap::core::KeySymbol;
using halocap::core::KeySymbolFromName;
using halocap::core::KeySymbolName;
using halocap::core::MatchesHotkey;
using halocap::core::ParseHotkeyText;
using halocap::core::SwallowDecision;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckText(const std::string& actual, const char* expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, actual.c_str(), expected);
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

KeyEvent Down(KeySymbol key, std::uint32_t modifiers) {
    KeyEvent event{};
    event.key = key;
    event.heldModifiers = modifiers;
    event.isKeyDown = true;
    return event;
}

void TestNameTable() {
    // 名字表是按编号区间生成的，所以「每个符号都有名字、且名字往回解析得到同一个符号」
    // 必须整体查一遍：区间里错一个值，设置里那个键就永远显示不出来。
    int missing = 0;
    int mismatched = 0;
    for (std::int32_t value = 1; value <= static_cast<std::int32_t>(KeySymbol::Last); ++value) {
        const KeySymbol key = static_cast<KeySymbol>(value);
        const char* name = KeySymbolName(key);
        if (name == nullptr || *name == 0) {
            ++missing;
            continue;
        }
        KeySymbol back = KeySymbol::None;
        if (!KeySymbolFromName(name, back) || back != key) {
            ++mismatched;
        }
    }
    CheckEqual(missing, 0, "every symbol in 1..Last has a name");
    CheckEqual(mismatched, 0, "every name parses back to the same symbol");

    CheckEqual(static_cast<long long>(KeySymbol::Last) - static_cast<long long>(KeySymbol::A) + 1,
               76, "the table covers 76 symbols");
    CheckText(KeySymbolName(KeySymbol::A), "A", "name of A");
    CheckText(KeySymbolName(KeySymbol::Z), "Z", "name of Z（区间的另一端必须落在 26）");
    CheckText(KeySymbolName(KeySymbol::Num0), "0", "name of 0");
    CheckText(KeySymbolName(KeySymbol::Num9), "9", "name of 9");
    CheckText(KeySymbolName(KeySymbol::F1), "F1", "name of F1");
    CheckText(KeySymbolName(KeySymbol::F24), "F24", "name of F24");
    CheckText(KeySymbolName(KeySymbol::PrintScreen), "PrintScreen", "name of PrintScreen");
    CheckText(KeySymbolName(KeySymbol(999)), "", "an unknown symbol has no name");

    KeySymbol parsed = KeySymbol::None;
    CheckTrue(KeySymbolFromName("A", parsed) && parsed == KeySymbol::A, "parse A");
    CheckTrue(KeySymbolFromName("f7", parsed) && parsed == KeySymbol::F7, "parse lowercase f7");
    CheckTrue(KeySymbolFromName(" Esc ", parsed) && parsed == KeySymbol::Escape, "parse Esc with spaces");
    CheckFalse(KeySymbolFromName("Ctrl", parsed), "a modifier alone is not a key");
    CheckFalse(KeySymbolFromName("", parsed), "empty is not a key");
    CheckFalse(KeySymbolFromName("€", parsed), "an unmapped symbol is refused");
}

void TestParse() {
    HotkeySpec spec{};

    CheckTrue(ParseHotkeyText("Ctrl+Alt+K", spec), "parse Ctrl+Alt+K");
    CheckEqual(static_cast<long long>(spec.key), static_cast<long long>(KeySymbol::K), "key is K");
    CheckEqual(spec.modifiers & halocap::core::kModControl, halocap::core::kModControl,
               "Ctrl bit set");
    CheckEqual(spec.modifiers & halocap::core::kModAlt, halocap::core::kModAlt, "Alt bit set");
    CheckEqual(spec.modifiers & halocap::core::kModShift, 0, "Shift not set");

    CheckTrue(ParseHotkeyText("F13", spec), "bare key parses");
    CheckEqual(spec.modifiers, 0, "bare key has no modifiers");

    // 1.x 存过 Control / MENU / SUPER 这些写法，宽容是刻意的。
    CheckTrue(ParseHotkeyText("Control+MENU+super+shift+F1", spec), "verbose modifier names");
    CheckEqual(spec.modifiers & (halocap::core::kModControl | halocap::core::kModAlt |
                                 halocap::core::kModWin | halocap::core::kModShift),
               halocap::core::kModControl | halocap::core::kModAlt | halocap::core::kModWin |
                   halocap::core::kModShift,
               "all four state bits");

    CheckTrue(ParseHotkeyText(" Ctrl + Shift + P ", spec), "spaces around pieces");
    CheckTrue(ParseHotkeyText("Ctrl+Alt+Shift+F13+NoRepeat", spec), "NoRepeat parses");
    CheckEqual(spec.modifiers & halocap::core::kModNoRepeat, halocap::core::kModNoRepeat,
               "NoRepeat bit");

    // 坏输入要拒而不是猜：设置文件是用户能手工编辑的。
    CheckFalse(ParseHotkeyText("", spec), "empty text");
    CheckFalse(ParseHotkeyText("Ctrl+", spec), "trailing separator");
    CheckFalse(ParseHotkeyText("+K", spec), "leading separator");
    CheckFalse(ParseHotkeyText("Ctrl+A+B", spec), "two keys");
    CheckFalse(ParseHotkeyText("Ctrl+€", spec), "unknown key name");
    CheckFalse(ParseHotkeyText("Hyper+K", spec), "unknown modifier");
    CheckFalse(ParseHotkeyText("Ctrl", spec), "modifiers only");
}

void TestFormat() {
    HotkeySpec spec{};
    CheckText(FormatHotkeyText(spec), "", "invalid spec formats to empty");

    ParseHotkeyText("Ctrl+Alt+Shift+Win+F9", spec);
    // 规范顺序是 Ctrl、Alt、Shift、Win——显示与写文件用同一个顺序，才能拿字符串比较设置。
    CheckText(FormatHotkeyText(spec), "Ctrl+Alt+Shift+Win+F9", "format with all modifiers");

    ParseHotkeyText("K", spec);
    CheckText(FormatHotkeyText(spec), "K", "bare key format");

    ParseHotkeyText("Shift+Enter", spec);
    CheckText(FormatHotkeyText(spec), "Shift+Enter", "special key format");

    // 来回换算必须稳定：设置页保存再读回来不能变样。
    HotkeySpec again{};
    ParseHotkeyText(FormatHotkeyText(spec), again);
    CheckText(FormatHotkeyText(again), "Shift+Enter", "round trip is stable");

    ParseHotkeyText("ctrl+alt+k", spec);
    CheckText(FormatHotkeyText(spec), "Ctrl+Alt+K", "lowercase input normalises on output");
}

void TestMatch() {
    const HotkeySpec radial = [] {
        HotkeySpec spec{};
        ParseHotkeyText("F13", spec);
        return spec;
    }();

    const HotkeySpec combo = [] {
        HotkeySpec spec{};
        ParseHotkeyText("Ctrl+Alt+K", spec);
        return spec;
    }();

    CheckTrue(MatchesHotkey(Down(KeySymbol::F13, 0), radial), "bare F13 hits the single-key hotkey");

    // 1.x 的那条老坑：单键热键不能被「按住 Ctrl 的 F13」触发。
    CheckFalse(MatchesHotkey(Down(KeySymbol::F13, halocap::core::kModControl), radial),
               "extra Ctrl must not fire a single-key hotkey");
    CheckFalse(MatchesHotkey(Down(KeySymbol::F13, halocap::core::kModShift |
                                        halocap::core::kModAlt), radial),
               "any extra modifier must not fire it either");

    CheckTrue(MatchesHotkey(Down(KeySymbol::K, halocap::core::kModControl |
                                           halocap::core::kModAlt), combo),
              "Ctrl+Alt+K hits the combo");
    CheckFalse(MatchesHotkey(Down(KeySymbol::K, halocap::core::kModControl), combo),
               "Ctrl alone is not the combo");
    CheckFalse(MatchesHotkey(Down(KeySymbol::K, halocap::core::kModControl | halocap::core::kModAlt |
                                           halocap::core::kModShift), combo),
               "an extra Shift is not the combo");
    CheckFalse(MatchesHotkey(Down(KeySymbol::J, halocap::core::kModControl | halocap::core::kModAlt),
                             combo),
               "a different key is not the combo");

    // 抬起永远不算触发。
    KeyEvent up = Down(KeySymbol::K, halocap::core::kModControl | halocap::core::kModAlt);
    up.isKeyDown = false;
    CheckFalse(MatchesHotkey(up, combo), "key up never matches");

    // NoRepeat 只在带这个位的时候才有意义。
    HotkeySpec noRepeat{};
    ParseHotkeyText("Ctrl+Alt+J+NoRepeat", noRepeat);
    KeyEvent repeat = Down(KeySymbol::J, halocap::core::kModControl | halocap::core::kModAlt);
    repeat.isRepeat = true;
    CheckFalse(MatchesHotkey(repeat, noRepeat), "NoRepeat swallows the auto repeat");

    const HotkeySpec allowRepeat = [] {
        HotkeySpec spec{};
        ParseHotkeyText("Ctrl+Alt+J", spec);
        return spec;
    }();
    CheckTrue(MatchesHotkey(repeat, allowRepeat), "without NoRepeat the repeat still matches");
}

void TestSwallow() {
    const HotkeySpec combo = [] {
        HotkeySpec spec{};
        ParseHotkeyText("Ctrl+Alt+K", spec);
        return spec;
    }();
    const std::vector<HotkeySpec> specs = { combo };

    CheckEqual(static_cast<long long>(DecideSwallow(
                   Down(KeySymbol::K, halocap::core::kModControl | halocap::core::kModAlt), specs)),
               static_cast<long long>(SwallowDecision::Swallow), "a matched hotkey is swallowed");

    CheckEqual(static_cast<long long>(DecideSwallow(
                   Down(KeySymbol::K, halocap::core::kModControl), specs)),
               static_cast<long long>(SwallowDecision::Pass), "a near miss passes through");

    KeyEvent up = Down(KeySymbol::K, halocap::core::kModControl | halocap::core::kModAlt);
    up.isKeyDown = false;
    CheckEqual(static_cast<long long>(DecideSwallow(up, specs)),
               static_cast<long long>(SwallowDecision::Pass),
               "the up event must pass or the foreground keeps a stuck modifier");

    KeyEvent repeat = up;
    repeat.isKeyDown = true;
    repeat.isRepeat = true;
    CheckEqual(static_cast<long long>(DecideSwallow(repeat, specs)),
               static_cast<long long>(SwallowDecision::Pass),
               "auto repeat passes: holding the key must not lock the foreground out");

    KeyEvent injected = Down(KeySymbol::K, halocap::core::kModControl | halocap::core::kModAlt);
    injected.injected = true;
    CheckEqual(static_cast<long long>(DecideSwallow(injected, specs)),
               static_cast<long long>(SwallowDecision::Pass),
               "injected input is never swallowed");

    KeyEvent lowIl = injected;
    lowIl.injected = false;
    lowIl.injectedByLowerIntegrity = true;
    CheckEqual(static_cast<long long>(DecideSwallow(lowIl, specs)),
               static_cast<long long>(SwallowDecision::Pass),
               "lower-integrity injection is never swallowed either");

    CheckEqual(static_cast<long long>(DecideSwallow(
                   Down(KeySymbol::K, halocap::core::kModControl | halocap::core::kModAlt), {})),
               static_cast<long long>(SwallowDecision::Pass), "an empty interception list swallows nothing");
}

}

int main() {
    // 崩了就什么都没有：行缓冲会在异常退出时把整段现场带走。
    setvbuf(stdout, nullptr, _IONBF, 0);

    TestNameTable();
    TestParse();
    TestFormat();
    TestMatch();
    TestSwallow();

    if (failures != 0) {
        std::printf("INPUT_POLICY_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("INPUT_POLICY_TESTS=OK checks=%d\n", checks);
    return 0;
}
