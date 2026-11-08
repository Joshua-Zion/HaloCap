#include "tray_menu.h"

namespace halocap::core {

namespace {

struct ActionEntry {
    AppAction action;
    const char* title;
};

// 顺序就是圆盘上的顺序，也是托盘菜单里的顺序。
const ActionEntry kActions[] = {
    { AppAction::Screenshot, "截图" },
    { AppAction::Ocr, "文字识别" },
    { AppAction::Record, "录屏" },
    { AppAction::Pin, "贴图" },
};

const char* ShortcutFor(const TrayMenuState& state, AppAction action) {
    switch (action) {
    case AppAction::Screenshot: return state.screenshotHotkey.c_str();
    case AppAction::Ocr: return state.ocrHotkey.c_str();
    case AppAction::Record: return state.recordHotkey.c_str();
    case AppAction::Pin: return state.pinHotkey.c_str();
    }
    return "";
}

TrayCommand CommandFor(AppAction action) {
    switch (action) {
    case AppAction::Screenshot: return TrayCommand::Screenshot;
    case AppAction::Ocr: return TrayCommand::Ocr;
    case AppAction::Record: return TrayCommand::Record;
    case AppAction::Pin: return TrayCommand::Pin;
    }
    return TrayCommand::None;
}

TrayMenuItem MakeItem(TrayCommand command, const char* label, const char* shortcut, bool enabled,
                      bool checked = false) {
    TrayMenuItem item;
    item.command = command;
    item.label = label;
    item.shortcut = shortcut;
    item.enabled = enabled;
    item.checked = checked;
    return item;
}

TrayMenuItem MakeSeparator() {
    TrayMenuItem item;
    item.separator = true;
    return item;
}

}  // namespace

const char* AppActionTitle(AppAction action) {
    for (const ActionEntry& entry : kActions) {
        if (entry.action == action) {
            return entry.title;
        }
    }
    return "";
}

bool AppActionFromIndex(std::int32_t index, AppAction& out) {
    if (index < 0 || index >= kAppActionCount) {
        return false;
    }
    out = static_cast<AppAction>(index);
    return true;
}

bool IsTrayCommand(std::int32_t id) {
    return id >= static_cast<std::int32_t>(TrayCommand::Screenshot) &&
           id <= static_cast<std::int32_t>(TrayCommand::Exit);
}

std::vector<TrayMenuItem> BuildTrayMenu(const TrayMenuState& state) {
    std::vector<TrayMenuItem> items;
    items.reserve(12);

    for (const ActionEntry& entry : kActions) {
        const bool stopping = entry.action == AppAction::Record && state.recording;
        // 1.x 的规则原样搬过来：录制中只有「停止录制」这一项是亮的。
        const bool enabled =
            stopping || (!state.recording && (entry.action != AppAction::Pin || state.pinAvailable));

        items.push_back(MakeItem(CommandFor(entry.action),
                                 stopping ? "停止录制" : entry.title,
                                 ShortcutFor(state, entry.action), enabled));

        if (stopping) {
            // 停止下面紧跟暂停/继续；它只在录制中出现，所以编号不变也有唯一含义。
            items.push_back(MakeItem(TrayCommand::PauseRecording,
                                     state.recordingPaused ? "继续录制" : "暂停录制", "", true));
        }
    }

    items.push_back(MakeSeparator());
    items.push_back(MakeItem(TrayCommand::OpenRadial, "打开光圈", state.radialHotkey.c_str(),
                             !state.recording));
    items.push_back(MakeItem(TrayCommand::Settings, "设置…", "", !state.recording));

    items.push_back(MakeSeparator());
    items.push_back(
        MakeItem(TrayCommand::ToggleAutoStart, "开机自启", "", !state.recording, state.autoStart));

    items.push_back(MakeSeparator());
    items.push_back(MakeItem(TrayCommand::Exit, "退出 HaloCap", "", true));

    return items;
}

std::string TrayTooltip(const TrayMenuState& state) {
    std::string tooltip;
    if (state.recording) {
        tooltip = state.recordingPaused ? "HaloCap · 录制已暂停" : "HaloCap · 正在录制";
    } else if (!state.radialHotkey.empty()) {
        tooltip = "HaloCap · " + state.radialHotkey;
    } else {
        tooltip = "HaloCap 光圈";
    }
    return TruncateTooltipUtf16(tooltip, kTrayTooltipMaxUnits);
}

std::string TruncateTooltipUtf16(std::string_view utf8, int maxUnits) {
    if (maxUnits <= 0) {
        return {};
    }

    std::string out;
    out.reserve(utf8.size());

    std::size_t index = 0;
    int units = 0;
    while (index < utf8.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8[index]);

        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        if (lead < 0x80u) {
            length = 1;
            codePoint = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            length = 2;
            codePoint = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            length = 3;
            codePoint = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            length = 4;
            codePoint = lead & 0x07u;
        } else {
            // 非法首字节：从这里截断，输出永远是合法 UTF-8。
            break;
        }

        // 序列被截断或后续字节不是 10xxxxxx：同样在这里截断，不把半个字交给 Shell。
        if (index + length > utf8.size()) {
            break;
        }
        bool valid = true;
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char continuation = static_cast<unsigned char>(utf8[index + i]);
            if ((continuation & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3Fu);
        }
        if (!valid) {
            break;
        }

        // BMP 以内占 1 个 UTF-16 码元，其余占 2 个（代理对）。
        const int cost = codePoint > 0xFFFFu ? 2 : 1;
        if (units + cost > maxUnits) {
            break;
        }

        out.append(utf8.substr(index, length));
        units += cost;
        index += length;
    }

    return out;
}

}
