// 设置 seam 的一次跑通：真实路径在哪、原子替换真的换上了、日志按规则分份、
// 以及**只读**导入本机那份 1.x settings.json 看它被读成了什么。
//
// 除了往 2.0 自己的日志目录写一行以外，这个探针不改动磁盘上的任何用户数据；
// 1.x 的文件只读，并且会比对读之前与读之后的大小与时间戳。
#include "win32_capability.h"
#include "win32_store.h"

#include "capability.h"
#include "settings.h"
#include "store.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace core = halocap::core;
using halocap::platform::win32::Win32Capability;
using halocap::platform::win32::Win32Store;

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

std::wstring Environment(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    value.resize(written);
    return value;
}

bool ReadUtf8File(const std::wstring& path, std::string& text, bool& exists) {
    text.clear();
    exists = false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64LL << 20)) {
        CloseHandle(file);
        return false;
    }
    text.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t read = 0;
    while (read < text.size()) {
        const std::size_t remaining = text.size() - read;
        const DWORD chunk = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
        DWORD done = 0;
        if (!ReadFile(file, text.data() + read, chunk, &done, nullptr) || done == 0) {
            break;
        }
        read += done;
    }
    CloseHandle(file);
    exists = true;
    return true;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 大小 + 最后写入时间：用来证「只读导入」真的没碰过 1.x 的文件。
struct FileStamp {
    bool exists = false;
    std::int64_t sizeBytes = 0;
    std::int64_t writtenUnixSeconds = 0;
};

FileStamp StampOf(const std::wstring& path) {
    FileStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return stamp;
    }
    stamp.exists = true;
    stamp.sizeBytes = (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) |
                      static_cast<std::int64_t>(data.nFileSizeLow);
    const long long ticks = (static_cast<long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                            data.ftLastWriteTime.dwLowDateTime;
    stamp.writtenUnixSeconds = (ticks - 116444736000000000LL) / 10000000LL;
    return stamp;
}

bool SameStamp(const FileStamp& a, const FileStamp& b) {
    return a.exists == b.exists && a.sizeBytes == b.sizeBytes &&
           a.writtenUnixSeconds == b.writtenUnixSeconds;
}

std::string TodayStamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", static_cast<int>(now.wYear),
                  static_cast<int>(now.wMonth), static_cast<int>(now.wDay));
    return buffer;
}

std::int64_t NowUnixSeconds() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const long long ticks = (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return (ticks - 116444736000000000LL) / 10000000LL;
}

std::wstring TemporaryRoot() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    wchar_t name[64] = {};
    std::swprintf(name, 64, L"halocap-store-probe-%lu", GetCurrentProcessId());
    return std::wstring(temp) + name;
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    const std::size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(0, slash);
}

// 目录能不能写：建一个探针文件再删掉。不能写不是代码错——沙箱与只读盘都会这样，
// 所以这里先问一次，再决定临时根放哪儿。
bool CanWriteDirectory(const std::wstring& directory) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(directory), error);
    if (error) {
        return false;
    }

    const std::wstring probe = directory + L"\\.write-probe";
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    DeleteFileW(probe.c_str());
    return true;
}

void PrintPath(const char* key, const char* value) { std::printf("%s=%s\n", key, value); }

void PrintCount(const char* key, const std::vector<std::string>& values) {
    std::printf("%s=%zu", key, values.size());
    for (const std::string& value : values) {
        std::printf(" [%s]", value.c_str());
    }
    std::printf("\n");
}

// ---- 1) 真实路径：必须是 HaloCap2，绝不能落到 1.x 的 HaloCap ----

void TestRealPaths(Win32Store& store) {
    const std::string settingsPath = store.SettingsPath();
    const std::string logDirectory = store.LogDirectory();
    const std::string modelDirectory = store.ModelDirectory();

    PrintPath("SETTINGS_PATH", settingsPath.c_str());
    PrintPath("LOG_DIR", logDirectory.c_str());
    PrintPath("MODEL_DIR", modelDirectory.c_str());

    Check(!settingsPath.empty(), "settings path is not empty");
    Check(!logDirectory.empty(), "log directory is not empty");
    Check(!modelDirectory.empty(), "model directory is not empty");

    // 这条是整个 seam 的纪律：写自己的目录，读别人的。
    Check(settingsPath.find("HaloCap2") != std::string::npos, "settings path is under HaloCap2");
    Check(settingsPath.find("\\HaloCap\\") == std::string::npos, "settings path is not the 1.x directory");
    Check(logDirectory.find("HaloCap2") != std::string::npos, "log directory is under HaloCap2");
    Check(modelDirectory.find("HaloCap2") != std::string::npos, "model directory is under HaloCap2");
    Check(modelDirectory.find(core::kOcrModelVersion) != std::string::npos,
          "model directory carries the model version");
}

// ---- 2) 原子替换与坏文件退路（在临时根里做，不碰真实设置） ----

void TestRoundTrip(const std::wstring& root) {
    Win32Store store(root);
    std::printf("TEMP_SETTINGS_PATH=%s\n", store.SettingsPath());

    core::AppSettings defaults;
    const bool saved = core::SaveSettingsTo(store, defaults);
    if (!saved) {
        // 保存失败时把平台给的原因打出来：探针的价值有一半在「为什么失败」上。
        std::printf("SAVE_ERROR=%s code=%d\n", store.LastErrorKind(),
                    static_cast<int>(store.LastErrorCode()));
    }
    Check(saved, "default settings save");
    Check(FileExists(root + L"\\settings.json"), "settings file exists after save");
    Check(!FileExists(root + L"\\settings.json.tmp"), "no temporary file is left behind");

    std::string raw;
    bool exists = false;
    Check(store.LoadSettings(raw, exists), "raw load succeeds");
    Check(exists, "raw load reports the file exists");
    Check(raw.find("\"schemaVersion\": 2") != std::string::npos, "saved text carries schemaVersion 2");

    core::SettingsLoadResult loaded = core::LoadSettingsFrom(store);
    Check(loaded.fileExists, "load reports fileExists=true");
    Check(loaded.report.parseError.empty(), "saved file parses cleanly");
    Check(loaded.settings == defaults, "saved settings round-trip");

    // 第二次保存必须整体换掉旧内容，而不是追加或留下一个半成品。
    core::AppSettings changed = defaults;
    changed.menuSize = core::MenuSize::Large;
    changed.recording.frameRate = core::RecordingFrameRate::Sixty;
    Check(core::SaveSettingsTo(store, changed), "changed settings save");
    Check(!FileExists(root + L"\\settings.json.tmp"), "replace leaves no temporary file");
    core::SettingsLoadResult reloaded = core::LoadSettingsFrom(store);
    Check(reloaded.settings == changed, "atomic replace exposes the new content");
    Check(reloaded.settings.menuSize == core::MenuSize::Large, "replaced value is visible");

    // 坏文件：整份退回默认值，并把原因报出来（1.x 在这里只写日志、不告诉调用方）。
    Check(store.SaveSettings("{\"menuSize\":"), "a broken document can be written");
    core::SettingsLoadResult broken = core::LoadSettingsFrom(store);
    std::printf("BROKEN_PARSE_ERROR=%s\n", broken.report.parseError.c_str());
    Check(!broken.report.parseError.empty(), "broken file reports a parse error");
    Check(broken.settings == core::AppSettings{}, "broken file falls back to defaults");

    // 0 字节文件与「文件不存在」要分开：前者是异常，后者是首次启动。
    Check(store.SaveSettings(""), "an empty document can be written");
    core::SettingsLoadResult empty = core::LoadSettingsFrom(store);
    Check(empty.fileExists, "empty file still reports fileExists=true");
    Check(empty.report.parseError == "empty", "empty file reports empty");
}

// ---- 3) 真实日志目录：追加一行，按规则选文件 ----

void TestRealLog(Win32Store& store) {
    const std::string today = TodayStamp();
    const std::string line = "probe: store seam check\n";

    const std::string written = core::AppendLogLine(store, today, line);
    std::printf("LOG_WRITTEN=%s\n", written.empty() ? "(none)" : written.c_str());

    if (written.empty()) {
        // 写不进真实日志目录时只报告原因、不判失败：沙箱与只读配置都会这样，
        // 与采集探针遇到「复制权被占用」是同一类环境事实，不是代码错。
        std::printf("LOG_WRITE=blocked kind=%s\n", store.LastErrorKind());
        return;
    }

    const std::vector<core::LogFileEntry> logs = store.ListLogs();
    std::printf("LOG_FILES=%zu\n", logs.size());
    bool found = false;
    for (const core::LogFileEntry& entry : logs) {
        if (entry.name == written) {
            found = true;
            std::printf("LOG_FILE_SIZE=%lld\n", static_cast<long long>(entry.sizeBytes));
        }
    }
    Check(found, "the written log shows up in ListLogs");

    // 过期清理只删超过保留期的；刚写的那一条必须留下。
    const std::vector<std::string> pruned = core::PruneLogs(store, NowUnixSeconds());
    std::printf("LOG_PRUNED=%zu\n", pruned.size());
    bool survivor = false;
    for (const core::LogFileEntry& entry : store.ListLogs()) {
        if (entry.name == written) {
            survivor = true;
        }
    }
    Check(survivor, "a fresh log survives pruning");
}

// ---- 4) 只读导入 1.x 的 settings.json ----

void TestLegacyImport() {
    const std::wstring appData = Environment(L"APPDATA");
    if (appData.empty()) {
        std::printf("IMPORT_1X=SKIPPED no APPDATA\n");
        return;
    }

    const std::wstring legacyPath = appData + L"\\HaloCap\\settings.json";
    const FileStamp before = StampOf(legacyPath);
    if (!before.exists) {
        std::printf("IMPORT_1X=SKIPPED no 1.x settings at %s\n",
                    WideToUtf8(legacyPath).c_str());
        return;
    }

    std::string text;
    bool exists = false;
    Check(ReadUtf8File(legacyPath, text, exists), "the 1.x settings file is readable");
    Check(exists, "the 1.x settings file exists");

    core::AppSettings settings;
    core::SettingsReadReport report;
    core::ParseSettings(text, settings, report);

    std::printf("IMPORT_1X_FILE=%s\n", WideToUtf8(legacyPath).c_str());
    std::printf("IMPORT_1X_BYTES=%lld\n", static_cast<long long>(before.sizeBytes));
    std::printf("IMPORT_1X_PARSE_ERROR=%s\n", report.parseError.empty() ? "none" : report.parseError.c_str());
    std::printf("IMPORT_1X_SCHEMA_VERSION=%d\n", report.schemaVersion);
    PrintCount("IMPORT_1X_MIGRATED", report.migratedKeys);
    PrintCount("IMPORT_1X_IGNORED", report.ignoredKeys);
    PrintCount("IMPORT_1X_REPAIRED", report.repairedKeys);
    PrintCount("IMPORT_1X_DROPPED", report.droppedKeys);

    std::printf("IMPORT_1X_HOTKEY=%s\n", core::FormatHotkeyText(settings.radialHotkey).c_str());
    std::printf("IMPORT_1X_SCREENSHOT_HOTKEY=%s\n",
                core::FormatHotkeyText(settings.screenshotHotkey).c_str());
    std::printf("IMPORT_1X_OCR_HOTKEY=%s\n", core::FormatHotkeyText(settings.ocrHotkey).c_str());
    std::printf("IMPORT_1X_RECORD_HOTKEY=%s\n", core::FormatHotkeyText(settings.recordHotkey).c_str());
    std::printf("IMPORT_1X_PIN_HOTKEY=%s\n", core::FormatHotkeyText(settings.pinHotkey).c_str());
    std::printf("IMPORT_1X_MENU_SIZE=%s\n", core::MenuSizeName(settings.menuSize));
    std::printf("IMPORT_1X_FORMAT=%s\n", core::ScreenshotFormatName(settings.screenshotFormat));
    std::printf("IMPORT_1X_OCR_ENGINE=%s\n", core::OcrEngineName(settings.ocr.engine));
    std::printf("IMPORT_1X_DERIVED_FPS=%d\n", settings.recording.FramesPerSecond());
    std::printf("IMPORT_1X_DERIVED_AUDIO_KBPS=%lld\n",
                static_cast<long long>(settings.recording.AudioBitrateBitsPerSec() / 1000));

    // 1.x 的文件里确实有那些只读派生字段：它们要被识别成「忽略」，不是「多出来的键」。
    Check(report.parseError.empty(), "1.x settings parse without error");
    Check(report.repairedKeys.empty(), "1.x settings need no repairs on this machine");
    Check(report.droppedKeys.empty(), "1.x settings drop nothing");
    Check(report.migratedKeys.size() == 5, "all five 1.x hotkeys migrate");
    Check(report.ignoredKeys.size() == 4, "the four derived fields are ignored");

    // 读成 2.0 的模型之后，再写再读必须回到同一份设置（这才叫「能导入」）。
    const std::string converted = core::SerializeSettings(settings);
    core::AppSettings again;
    core::SettingsReadReport againReport;
    core::ParseSettings(converted, again, againReport);
    Check(again == settings, "imported settings survive a 2.0 round-trip");
    Check(againReport.parseError.empty(), "the 2.0 round-trip parses");
    Check(againReport.migratedKeys.empty(), "the 2.0 round-trip needs no migration");

    // 只读：整份导入过程不能改动 1.x 的文件（大小与时间戳都要原样）。
    const FileStamp after = StampOf(legacyPath);
    Check(SameStamp(before, after), "the 1.x settings file is untouched");
}

// ---- 5) 能力 seam：系统下限、磁盘下限、空闲回收线 ----

void TestCapability() {
    Win32Capability capability;
    const core::PlatformFacts facts = capability.Probe();
    const core::FeatureFlags flags = core::EvaluateFeatures(facts);

    std::printf("OS_BUILD=%d\n", facts.osBuild);
    std::printf("MEETS_FLOOR=%d\n", flags.recording ? 1 : 0);
    std::printf("CAPTURE_TOGGLE=%d\n", flags.cursorToggle ? 1 : 0);
    std::printf("WGC_SUPPORTED=%d\n", facts.graphicsCaptureSupported ? 1 : 0);
    std::printf("WINDOW_RECORDING=%d\n", flags.windowRecording ? 1 : 0);
    std::printf("PRIVATE_MB=%.1f\n", static_cast<double>(facts.privateMemoryBytes) / (1024.0 * 1024.0));
    std::printf("IDLE_TRIM=%d\n", flags.needsIdleTrim ? 1 : 0);
    std::printf("FLOOR_MESSAGE=%s\n", core::DescribeWindowsFloor(facts.osBuild).c_str());

    Check(facts.osBuild > 0, "the OS build is readable");

    // 磁盘：用 1.x 的默认录制目录（我的视频\Captures）问一次。
    const std::wstring videos = Environment(L"USERPROFILE");
    const std::wstring captures = videos.empty() ? std::wstring{} : videos + L"\\Videos\\Captures";
    const std::string capturesUtf8 = WideToUtf8(captures);
    const std::int64_t available = capability.AvailableBytes(capturesUtf8.c_str());

    constexpr std::int64_t kBalanced4K30Bitrate = 27'371'520;
    const core::DiskSpaceVerdict verdict = core::EvaluateDiskSpace(available, kBalanced4K30Bitrate);

    std::printf("DISK_AVAILABLE_MB=%.1f\n", static_cast<double>(available) / (1024.0 * 1024.0));
    std::printf("DISK_REQUIRED_MB=%.1f\n",
                static_cast<double>(verdict.requiredBytes) / (1024.0 * 1024.0));
    std::printf("DISK_OK=%d\n", verdict.enough ? 1 : 0);
    Check(verdict.requiredBytes == 205'286'400, "4K30 balanced needs one minute of bitrate");

    // 探针自己是小进程，不该过空闲回收线；过了说明这台机器上已经有别的东西在占内存，
    // 那本身也是要打印出来的事实，不能判 FAIL。
    std::printf("IDLE_THRESHOLD_MB=%.1f\n",
                static_cast<double>(core::kIdleTrimThresholdBytes) / (1024.0 * 1024.0));
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    Win32Store realStore;
    TestRealPaths(realStore);

    // 临时根默认在 %TEMP%；传一个参数就换成它（排查「写不进去」时用得到）。
    // %TEMP% 不可写时（沙箱、只读配置）退到 exe 旁边的目录——探针的承诺是
    // 「只在 2.0 自己的目录里写」，不是「一定写在 %TEMP%」。
    std::wstring tempRoot = argc > 1 ? Utf8ToWide(argv[1]) : TemporaryRoot();
    const char* rootSource = argc > 1 ? "argument" : "temp";
    if (argc <= 1 && !CanWriteDirectory(tempRoot)) {
        tempRoot = ExecutableDirectory() + L"\\store-probe-root";
        rootSource = "exe-dir";
    }
    std::printf("TEMP_ROOT=%s\n", WideToUtf8(tempRoot).c_str());
    std::printf("TEMP_ROOT_SOURCE=%s\n", rootSource);
    TestRoundTrip(tempRoot);
    TestRealLog(realStore);
    TestLegacyImport();
    TestCapability();

    // 临时根只留最后一次跑的证据？不留：探针跑完就清掉，和它写东西的承诺一致。
    {
        std::error_code ignored;
        std::filesystem::remove_all(std::filesystem::path(tempRoot), ignored);
        std::printf("TEMP_CLEANED=%d\n", FileExists(tempRoot) ? 0 : 1);
    }

    std::printf("STORE_PROBE_CHECKS=%d\n", checks);
    if (failures != 0) {
        std::printf("STORE_PROBE=FAILED %d\n", failures);
        return 1;
    }

    std::printf("STORE_PROBE=OK\n");
    return 0;
}
