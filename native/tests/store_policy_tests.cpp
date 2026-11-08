// IStore 之上那层 core 逻辑：坏文件怎么退、日志写哪一份、过期日志删哪些。
// 用一个内存里的假 store 跑，不需要真的碰磁盘——磁盘那一半在 tools/store_probe 里。
#include "store.h"

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
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

void CheckText(const std::string& actual, const std::string& expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, actual.c_str(), expected.c_str());
    }
}

class FakeStore final : public core::IStore {
public:
    struct LogFile {
        std::string text;
        std::int64_t sizeBytes = 0;
        std::int64_t writtenUnixSeconds = 0;
    };

    bool loadOk = true;
    bool saveOk = true;
    bool appendOk = true;

    bool settingsExists = false;
    std::string settingsText;
    std::string errorKind = "none";

    std::int64_t nowUnixSeconds = 1'800'000'000;
    std::map<std::string, LogFile> logs;

    void SeedLog(const std::string& name, std::int64_t sizeBytes, std::int64_t writtenUnixSeconds) {
        LogFile file;
        file.sizeBytes = sizeBytes;
        file.writtenUnixSeconds = writtenUnixSeconds;
        logs[name] = file;
    }

    bool LoadSettings(std::string& text, bool& exists) override {
        if (!loadOk) {
            errorKind = "denied";
            return false;
        }
        text = settingsText;
        exists = settingsExists;
        errorKind = settingsExists ? "ok" : "missing";
        return true;
    }

    bool SaveSettings(std::string_view text) override {
        if (!saveOk) {
            errorKind = "denied";
            return false;
        }
        settingsText = std::string(text);
        settingsExists = true;
        errorKind = "ok";
        return true;
    }

    bool AppendLog(std::string_view name, std::string_view line) override {
        if (!appendOk) {
            errorKind = "denied";
            return false;
        }
        LogFile& file = logs[std::string(name)];
        file.text.append(line);
        file.sizeBytes += static_cast<std::int64_t>(line.size());
        file.writtenUnixSeconds = nowUnixSeconds;
        errorKind = "ok";
        return true;
    }

    std::vector<core::LogFileEntry> ListLogs() override {
        std::vector<core::LogFileEntry> out;
        for (const auto& [name, file] : logs) {
            out.push_back({ name, file.sizeBytes, file.writtenUnixSeconds });
        }
        return out;
    }

    bool DeleteLog(std::string_view name) override {
        logs.erase(std::string(name));
        return true;
    }

    const char* SettingsPath() const override { return "mem://settings.json"; }
    const char* LogDirectory() const override { return "mem://logs"; }
    const char* ModelDirectory() const override { return "mem://models"; }
    const char* LastErrorKind() const override { return errorKind.c_str(); }
    std::int32_t LastErrorCode() const override { return 0; }
};

void TestLoadMissingFile() {
    FakeStore store;
    core::SettingsLoadResult result = core::LoadSettingsFrom(store);

    CheckTrue(!result.fileExists, "missing file reports fileExists=false");
    CheckTrue(result.report.parseError.empty(), "missing file is not a parse error");
    CheckTrue(result.settings == core::AppSettings{}, "missing file yields defaults");
}

void TestLoadFailure() {
    FakeStore store;
    store.loadOk = false;

    core::SettingsLoadResult result = core::LoadSettingsFrom(store);
    CheckTrue(!result.fileExists, "unreadable file reports fileExists=false");
    CheckText(result.report.parseError, "denied", "unreadable file reports the platform error kind");
    CheckTrue(result.settings == core::AppSettings{}, "unreadable file yields defaults");
}

void TestLoadExisting() {
    FakeStore store;
    store.settingsExists = true;
    store.settingsText = R"({"menuSize":"Large","recording":{"frameRate":60}})";

    core::SettingsLoadResult result = core::LoadSettingsFrom(store);
    CheckTrue(result.fileExists, "existing file reports fileExists=true");
    CheckTrue(result.report.parseError.empty(), "valid file parses");
    CheckTrue(result.settings.menuSize == core::MenuSize::Large, "value is applied");
    CheckTrue(result.settings.recording.frameRate == core::RecordingFrameRate::Sixty,
              "nested value is applied");
}

void TestLoadEmptyFile() {
    FakeStore store;
    store.settingsExists = true;
    store.settingsText.clear();

    core::SettingsLoadResult result = core::LoadSettingsFrom(store);
    CheckTrue(result.fileExists, "empty file still exists");
    // 0 字节文件是「写到一半被杀掉」留下的形状，要报出来，而不是当成首次启动。
    CheckText(result.report.parseError, "empty", "empty file reports empty");
    CheckTrue(result.settings == core::AppSettings{}, "empty file yields defaults");
}

void TestLoadBrokenFile() {
    FakeStore store;
    store.settingsExists = true;
    store.settingsText = "{\"menuSize\":";

    core::SettingsLoadResult result = core::LoadSettingsFrom(store);
    CheckText(result.report.parseError, "unterminated", "broken file reports the parser error");
    CheckTrue(result.settings == core::AppSettings{}, "broken file yields defaults");
}

void TestSave() {
    FakeStore store;
    core::AppSettings settings;
    settings.menuSize = core::MenuSize::Small;
    settings.recordingDirectory = "D:\\Captures";

    CheckTrue(core::SaveSettingsTo(store, settings), "save succeeds");
    CheckTrue(store.settingsExists, "save creates the file");

    // 存进去的文本必须能被读回同一份设置——这是原子写之外唯一重要的事。
    core::SettingsLoadResult reloaded = core::LoadSettingsFrom(store);
    CheckTrue(reloaded.settings == settings, "saved settings round-trip through the store");

    store.saveOk = false;
    CheckTrue(!core::SaveSettingsTo(store, settings), "save failure is reported");
    CheckText(store.errorKind, "denied", "save failure keeps the platform error kind");
}

void TestAppendLog() {
    FakeStore store;
    CheckText(core::AppendLogLine(store, "20261108", "hello\n"), "app-20261108.log",
              "first line goes to the first part");
    CheckEqual(static_cast<long long>(store.logs.size()), 1, "one log file was created");
    CheckText(store.logs["app-20261108.log"].text, "hello\n", "line content is written");

    CheckText(core::AppendLogLine(store, "20261108", "again\n"), "app-20261108.log",
              "a second line stays in the same part");
    CheckText(store.logs["app-20261108.log"].text, "hello\nagain\n", "lines accumulate");

    // 别的日期不参与今天的选择。
    store.SeedLog("app-20260101.log", 1024, store.nowUnixSeconds);
    CheckText(core::AppendLogLine(store, "20261108", "x\n"), "app-20261108.log",
              "other days do not count as today");

    // 不是日志的文件（人手放的、别的工具的）也不该算进份数。
    store.SeedLog("notes.txt", core::kLogMaxFileBytes, store.nowUnixSeconds);
    CheckText(core::AppendLogLine(store, "20261108", "y\n"), "app-20261108.log",
              "foreign files are ignored");
}

void TestAppendLogRotation() {
    FakeStore store;
    store.SeedLog("app-20261108.log", core::kLogMaxFileBytes, store.nowUnixSeconds);

    CheckText(core::AppendLogLine(store, "20261108", "second part\n"), "app-20261108-2.log",
              "a full part rolls to the next one");

    // 六份全满：本日不再写，调用方拿 "" 就该知道要提示「清理日志目录」。
    FakeStore full;
    for (int part = 0; part < core::kLogMaxPartsPerDay; ++part) {
        full.SeedLog(core::LogFileName("20261108", part), core::kLogMaxFileBytes, full.nowUnixSeconds);
    }
    CheckText(core::AppendLogLine(full, "20261108", "nope\n"), "", "capacity stops writing");
    CheckEqual(static_cast<long long>(full.logs.size()), core::kLogMaxPartsPerDay,
               "capacity does not create a seventh file");
}

void TestPruneLogs() {
    constexpr std::int64_t kDay = 86400;
    FakeStore store;
    store.SeedLog("app-20200101.log", 100, store.nowUnixSeconds - 20 * kDay);
    store.SeedLog("app-20200102.log", 100, store.nowUnixSeconds - 1 * kDay);

    std::vector<std::string> deleted = core::PruneLogs(store, store.nowUnixSeconds);
    CheckEqual(static_cast<long long>(deleted.size()), 1, "one expired log is deleted");
    CheckText(deleted.empty() ? std::string{} : deleted[0], "app-20200101.log", "the old one is deleted");
    CheckEqual(static_cast<long long>(store.logs.size()), 1, "the recent log survives");
    CheckTrue(store.logs.count("app-20200102.log") == 1, "the recent log is still there");
}

}

int main() {
    TestLoadMissingFile();
    TestLoadFailure();
    TestLoadExisting();
    TestLoadEmptyFile();
    TestLoadBrokenFile();
    TestSave();
    TestAppendLog();
    TestAppendLogRotation();
    TestPruneLogs();

    if (failures != 0) {
        std::printf("STORE_POLICY_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("STORE_POLICY_TESTS=OK checks=%d\n", checks);
    return 0;
}
