#include "store.h"

#include <algorithm>

namespace halocap::core {

SettingsLoadResult LoadSettingsFrom(IStore& store) {
    SettingsLoadResult result;

    std::string text;
    bool exists = false;
    if (!store.LoadSettings(text, exists)) {
        // 读不到不等于「设置内容坏了」：退回默认值，并把原因报出来。
        // 1.x 在这里是一整段 try/catch，行为相同，只是不告诉调用方为什么。
        result.report.parseError = store.LastErrorKind();
        return result;
    }

    result.fileExists = exists;
    if (!exists) {
        // 首次启动：默认值，且不是错误。
        return result;
    }

    // 文件存在但是 0 字节，那是「写到一半被杀掉」留下的形状（原子替换之后应该不再出现，
    // 但旧版本或人手清空的文件会）。照样走解析器，让它报 empty。
    ParseSettings(text, result.settings, result.report);
    return result;
}

bool SaveSettingsTo(IStore& store, const AppSettings& settings) {
    return store.SaveSettings(SerializeSettings(settings));
}

std::string AppendLogLine(IStore& store, std::string_view day, std::string_view line) {
    std::vector<LogPartSize> today;
    for (const LogFileEntry& entry : store.ListLogs()) {
        LogNameParts parts;
        if (!ParseLogFileName(entry.name, parts) || std::string_view(parts.day) != day) {
            continue;
        }
        today.push_back({ parts.part, entry.sizeBytes });
    }

    const LogTarget target = DecideLogTarget(day, today);
    if (target.atCapacity) {
        return {};
    }
    if (!store.AppendLog(target.name, line)) {
        return {};
    }
    return target.name;
}

std::vector<std::string> PruneLogs(IStore& store, std::int64_t nowUnixSeconds) {
    std::vector<std::string> deleted;
    for (const std::string& name : SelectExpiredLogs(store.ListLogs(), nowUnixSeconds, kLogRetainDays)) {
        if (store.DeleteLog(name)) {
            deleted.push_back(name);
        }
    }
    return deleted;
}

}
