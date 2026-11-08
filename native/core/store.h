#pragma once

#include "capability.h"
#include "settings.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// 内置 OCR 模型的目录版本。1.x 用它做「换模型就换目录」的隔离（老版本残留直接在
// LocalAppData 里并存），2.0 沿用同一个版本号，这样两台版本的模型文件可以一一对照。
constexpr const char* kOcrModelVersion = "ppocrv6-tiny-1";

// 8 个 seam 里的第八个（`IStore` + `ICapability`）的存储半边。
//
// 平台侧只管「一段 UTF-8 文本从一个位置原子地换进另一个位置」，以及「目录里有哪些文件」。
// 设置文件长什么样、日志留几份、模型放在哪一层，全是 core 的事——这条分工让 1.x 的
// settings.json 能被 2.0 直接读，也让 macOS 那天不用重写一遍设置。
//
// 一条纪律写在接口上：2.0 只写自己的目录，绝不碰 1.x 的 `%APPDATA%\HaloCap`。
// 1.x 的文件只读（导入），写回去的永远是 `%APPDATA%\HaloCap2`。
class IStore {
public:
    virtual ~IStore() = default;

    // 读整份设置文本。文件不存在不是错误：返回 true、exists=false、text 为空。
    virtual bool LoadSettings(std::string& text, bool& exists) = 0;

    // 原子替换：先写临时文件、刷盘，再整体换上去。失败时旧文件必须还在。
    virtual bool SaveSettings(std::string_view text) = 0;

    // 往指定文件名追加一行（文件名由 core 的日志规则给出，平台不判断该写哪一份）。
    virtual bool AppendLog(std::string_view name, std::string_view line) = 0;
    virtual std::vector<LogFileEntry> ListLogs() = 0;
    virtual bool DeleteLog(std::string_view name) = 0;

    virtual const char* SettingsPath() const = 0;
    virtual const char* LogDirectory() const = 0;
    virtual const char* ModelDirectory() const = 0;

    // "ok" / "missing" / "denied" / "io" / "invalid" / "none"
    virtual const char* LastErrorKind() const = 0;
    virtual std::int32_t LastErrorCode() const = 0;
};

struct SettingsLoadResult {
    AppSettings settings;
    SettingsReadReport report;

    // false = 首次启动（文件还不存在）。这种情况不是「设置坏了」，报告里不该有错误。
    bool fileExists = false;
};

// 一读、一修、出报告。坏文件的退法只在这里定一次，应用与探针走同一条路。
SettingsLoadResult LoadSettingsFrom(IStore& store);

// 序列化 + 原子写。失败时调用方仍持有内存里的设置，可以提示「这次没存上」。
bool SaveSettingsTo(IStore& store, const AppSettings& settings);

// 按日志规则写一行：选今天该写的那一份 → 追加。返回写到的文件名；"" = 本日已达上限。
std::string AppendLogLine(IStore& store, std::string_view day, std::string_view line);

// 删掉超过保留期的日志（规则见 capability.h），返回真正删掉的文件名。
std::vector<std::string> PruneLogs(IStore& store, std::int64_t nowUnixSeconds);

}
