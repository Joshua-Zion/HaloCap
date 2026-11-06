#include "input_host.h"

#include <algorithm>
#include <cctype>

namespace halocap::core {

namespace {

// 名字表按编号区间生成（字母 1–26、数字 27–36、F1–F24 是 37–60），剩下的特殊键逐条给；
// 区间的连续性与「每个符号都有名字」由 input_policy_tests 整表验证。
struct NameEntry {
    KeySymbol key;
    std::string name;
};

const std::vector<NameEntry>& NameTable() {
    static const std::vector<NameEntry> table = [] {
        std::vector<NameEntry> entries;

        for (int i = 0; i < 26; ++i) {
            entries.push_back({ static_cast<KeySymbol>(1 + i), std::string(1, static_cast<char>('A' + i)) });
        }
        for (int i = 0; i < 10; ++i) {
            entries.push_back({ static_cast<KeySymbol>(27 + i), std::string(1, static_cast<char>('0' + i)) });
        }
        for (int i = 1; i <= 24; ++i) {
            entries.push_back({ static_cast<KeySymbol>(36 + i), "F" + std::to_string(i) });
        }

        const std::pair<KeySymbol, const char*> specials[] = {
            { KeySymbol::Escape, "Esc" },         { KeySymbol::Enter, "Enter" },
            { KeySymbol::Space, "Space" },        { KeySymbol::Tab, "Tab" },
            { KeySymbol::Backspace, "Backspace" },{ KeySymbol::Insert, "Insert" },
            { KeySymbol::Delete, "Delete" },      { KeySymbol::Home, "Home" },
            { KeySymbol::End, "End" },            { KeySymbol::PageUp, "PageUp" },
            { KeySymbol::PageDown, "PageDown" },  { KeySymbol::Up, "Up" },
            { KeySymbol::Down, "Down" },          { KeySymbol::Left, "Left" },
            { KeySymbol::Right, "Right" },        { KeySymbol::PrintScreen, "PrintScreen" },
        };
        for (const auto& special : specials) {
            entries.push_back({ special.first, special.second });
        }

        return entries;
    }();

    return table;
}

struct ModifierName {
    std::uint32_t bit;
    const char* name;
};

const ModifierName kModifierNames[] = {
    { kModControl, "Ctrl" },
    { kModAlt, "Alt" },
    { kModShift, "Shift" },
    { kModWin, "Win" },
    { kModNoRepeat, "NoRepeat" },
};

// 修饰键的名字接受几种写法：设置页显示的是 "Ctrl"，但 1.x 存过 "Control"、
// 手工编辑配置文件的人写过 "CTRL"。解析要宽容，格式化只有一种规范输出。
bool ModifierBitFromName(std::string_view name, std::uint32_t& bit) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lowered == "ctrl" || lowered == "control") {
        bit = kModControl;
    } else if (lowered == "alt" || lowered == "menu") {
        bit = kModAlt;
    } else if (lowered == "shift") {
        bit = kModShift;
    } else if (lowered == "win" || lowered == "meta" || lowered == "super" ||
               lowered == "cmd") {
        bit = kModWin;
    } else if (lowered == "norepeat") {
        bit = kModNoRepeat;
    } else {
        return false;
    }
    return true;
}

std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}

const char* KeySymbolName(KeySymbol key) {
    for (const auto& entry : NameTable()) {
        if (entry.key == key) {
            return entry.name.c_str();
        }
    }
    return "";
}

bool KeySymbolFromName(std::string_view name, KeySymbol& out) {
    const std::string lowered = Lower(Trim(name));
    if (lowered.empty()) {
        return false;
    }

    for (const auto& entry : NameTable()) {
        if (lowered == Lower(entry.name)) {
            out = entry.key;
            return true;
        }
    }
    return false;
}

bool ParseHotkeyText(std::string_view text, HotkeySpec& out) {
    out = HotkeySpec{};

    std::string_view remainder = Trim(text);
    if (remainder.empty()) {
        return false;
    }

    // 按 '+' 切段；所有段都先按修饰键解释，剩下唯一一个不是修饰键的段才是键。
    // 键名里本身可能带 '+'（小键盘的 Plus），这里不支持——设置里没人这么写，
    // 但要支持就得规定转义，代价大于收益。
    std::string_view keyPart;
    bool keyTaken = false;

    while (!remainder.empty()) {
        const auto separator = remainder.find('+');
        std::string_view piece = separator == std::string_view::npos
                                     ? remainder
                                     : remainder.substr(0, separator);
        remainder = separator == std::string_view::npos
                        ? std::string_view{}
                        : remainder.substr(separator + 1);

        piece = Trim(piece);
        if (piece.empty()) {
            return false;   // "Ctrl+" 或 "++K" 都是坏输入，不接受
        }

        std::uint32_t bit = 0;
        if (ModifierBitFromName(piece, bit)) {
            out.modifiers |= bit;
            continue;
        }

        if (keyTaken) {
            return false;   // 两段都不是修饰键：那是 "Ctrl+A+B"
        }

        KeySymbol symbol = KeySymbol::None;
        if (!KeySymbolFromName(piece, symbol)) {
            return false;
        }
        out.key = symbol;
        keyTaken = true;
    }

    return keyTaken;
}

std::string FormatHotkeyText(const HotkeySpec& spec) {
    if (!spec.Valid()) {
        return {};
    }

    std::string text;
    for (const auto& entry : kModifierNames) {
        if ((spec.modifiers & entry.bit) != 0) {
            if (!text.empty()) {
                text += '+';
            }
            text += entry.name;
        }
    }

    if (!text.empty()) {
        text += '+';
    }
    text += KeySymbolName(spec.key);
    return text;
}

bool MatchesHotkey(const KeyEvent& event, const HotkeySpec& spec) {
    if (!spec.Valid() || event.key != spec.key) {
        return false;
    }

    // 只看状态位：NoRepeat 是注册请求，不是按键时按住的键。
    const std::uint32_t state = event.heldModifiers & kStateModifiers;
    const std::uint32_t wanted = spec.modifiers & kStateModifiers;
    if (state != wanted) {
        return false;
    }

    // 按住不放的自动重复不算第二次触发。
    if ((spec.modifiers & kModNoRepeat) != 0 && event.isRepeat) {
        return false;
    }

    return event.isKeyDown;
}

SwallowDecision DecideSwallow(const KeyEvent& event, const std::vector<HotkeySpec>& specs) {
    if (!event.isKeyDown || event.isRepeat) {
        // 抬起必须放过：吞掉 up 会让前台程序卡在「修饰键一直按着」的状态里。
        return SwallowDecision::Pass;
    }

    // 注入的事件一律放过。钩子里分不清「用户按的」与「我们自己发的」的时候，
    // 吞掉注入的会把自动化与远程桌面的输入吃掉。
    if (event.injected || event.injectedByLowerIntegrity) {
        return SwallowDecision::Pass;
    }

    for (const auto& spec : specs) {
        if (MatchesHotkey(event, spec)) {
            return SwallowDecision::Swallow;
        }
    }

    return SwallowDecision::Pass;
}

}
