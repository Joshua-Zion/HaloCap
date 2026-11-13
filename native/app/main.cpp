// S1 的组装：把 8 个 seam 接成一个能起来、能待着、能自己报出空闲内存的进程。
//
// 这一版刻意只做 S1 的骨架：设置、日志、托盘、热键、覆盖层窗口与渲染后端就绪，消息循环待着；
// 托盘菜单与热键只记录「收到了什么动作」——真正的截图/录屏/OCR/贴图在 S3/S4 才接。
// 它要回答的就是 S1 的最后一条验收：**空闲私有内存 ≤ 80 MB**（见计划 §1.3 与 §5）。
//
// 覆盖层按需创建（第一次截图才会用），所以「空闲」量两段：
//   APP_IDLE_*    托盘 + 热键 + 设置 + 日志，覆盖层还没建；
//   APP_OVERLAY_* 覆盖层窗口 + D2D/DComp 合成链挂上之后（这才是 spike 3 量到的 60 MB 那一段）。
// 两段都要在 80 MB 以内，`--selftest` 会把这条做成断言并给出退出码。
//
// 用法：
//   HaloCap.exe                              正常起（待着，托盘菜单里退出）
//   HaloCap.exe --selftest [--seconds 3]     起来 → 量空闲 → 建覆盖层 → 再量 → 退出
//   HaloCap.exe --selftest --report out.txt  把上面的每一项写进文件（explorer 启动时没有控制台）
//   --no-tray / --no-input / --no-overlay    只量某几层，用来归因
#include "capability.h"
#include "clipboard.h"
#include "desktop_grab.h"
#include "font_rasterizer.h"
#include "image_export.h"
#include "input_host.h"
#include "recording_session.h"
#include "settings.h"
#include "store.h"
#include "tray_menu.h"

#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"
#include "mf_h264_encoder.h"
#include "ocr_engine.h"
#include "ocr_model.h"
#include "ocr_result_window.h"
#include "ocr_worker.h"
#include "ort_ocr_engine.h"
#include "pin_window.h"
#include "radial_menu.h"
#include "recording_hud.h"
#include "screenshot_overlay.h"
#include "settings_window.h"
#include "tray_menu_window.h"
#include "update.h"
#include "version.h"
#include "wasapi_audio_source.h"
#include "win32_autostart.h"
#include "win32_capability.h"
#include "win32_clipboard.h"
#include "win32_input_host.h"
#include "win32_store.h"
#include "win32_surface_window.h"
#include "win32_update_fetch.h"
#include "win32_wgc_capture.h"
#include "window_at_point.h"
#include "window_picker.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include "win32_tray.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace core = halocap::core;
using halocap::platform::win32::D2dFontRasterizer;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::MfH264Encoder;
using halocap::platform::win32::WasapiAudioSource;
using halocap::platform::win32::Win32Capability;
using halocap::platform::win32::Win32DesktopDuplication;
using halocap::platform::win32::Win32InputHost;
using halocap::platform::win32::Win32Store;
using halocap::platform::win32::Win32SurfaceWindow;
using halocap::platform::win32::Win32WgcCapture;
using halocap::platform::win32::Win32Tray;

namespace {

// Win10 2004 起才有；老 SDK 头文件里没有这个常量（与 recording_hud.cpp 同一守卫）。
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

constexpr int kDefaultSelftestSeconds = 3;

// 热键 token：平台只负责带回来，含义在应用侧。
enum HotkeyToken : int {
    kTokenRadial = 1,
    kTokenScreenshot = 2,
    kTokenOcr = 3,
    kTokenRecord = 4,
    kTokenPin = 5,
};

// 下标 = core 的热键槽位序（0 光圈、1 截图、2 识别、3 录屏、4 贴图）。
constexpr int kHotkeyTokens[core::kHotkeySlotCount] = {
    kTokenRadial, kTokenScreenshot, kTokenOcr, kTokenRecord, kTokenPin
};
constexpr const char* kHotkeyNames[core::kHotkeySlotCount] = {
    "radial", "screenshot", "ocr", "record", "pin"
};
constexpr const wchar_t* kHotkeyLabels[core::kHotkeySlotCount] = {
    L"呼出光圈", L"截图", L"文字识别", L"录屏", L"贴图"
};

struct Options {
    bool selftest = false;
    int seconds = kDefaultSelftestSeconds;
    std::wstring reportPath;
    bool tray = true;
    bool input = true;
    bool overlay = true;
    bool help = false;

    // 0 = 用主显示器尺寸。量「空闲预算在哪个分辨率下还成立」时用得到。
    int overlayWidth = 0;
    int overlayHeight = 0;

    // >0 = 启动后立刻录这么多秒，然后退出。无人值守的端到端自检入口。
    int recordSeconds = 0;
    core::IntRect recordRegion{};
    bool hasRecordRegion = false;
    bool recordWindow = false;

    // 非空 = 无人值守的截图自检：开覆盖层 → 摆选区 → 标注 → 存 PNG + 贴图 → 退出。
    std::wstring screenshotSelftestPath;

    // 非空 = 无人值守的 OCR 自检：开 OCR 覆盖层 → 选区 → 识别 → 结果窗口 → 抓图。
    std::wstring ocrSelftestPath;

    // 非空 = 无人值守的设置页自检：打开六页设置 → 抓图 → 退出。
    std::wstring settingsSelftestPath;
    std::wstring ocrModelDownloadSelftestPath;
    std::wstring settingsClickSelftestPath;

    // 非空 = 无人值守的热键自检：注入真按键走一遍捕获/冲突/取消/保存闸门。
    std::wstring hotkeySelftestPath;

    // 非空 = 「改键之后真的能触发」自检：走 app 自己的设置窗 + 真实保存回调 + 真实注册。
    std::wstring hotkeyLiveSelftestPath;

    // 非空 = 无人值守的 OCR 搜索自检：喂合成识别结果，验计数/回绕/清空与图上高亮重绘。
    std::wstring ocrSearchSelftestPath;

    // 非空 = 无人值守的贴图自检：往剪贴板写颜色文本 → 贴图 → 抓图 → 退出。
    std::wstring pinSelftestPath;

    // 非空 = 无人值守的圆盘自检：打开圆盘 → 抓图 → 退出。
    std::wstring radialSelftestPath;
    std::wstring windowPickerSelftestPath;
    std::wstring radialClickSelftestPath;
    int settingsSelftestPage = -1;
    int settingsSelftestScroll = 0;

    // 非空 = 无人值守的录屏 UI 自检：边框/倒计时/浮层的位置、排除采集、按钮与抓图。
    std::wstring recordingUiSelftestPath;

    // 非空 = 无人值守的托盘菜单自检：自绘菜单的条目、点击、键盘与收起。
    std::wstring trayMenuSelftestPath;

    // 交付：--version 打印版本；--check-update 读一份清单并比较版本。
    bool version = false;
    std::wstring updateManifestPath;
    // --update-check：直接查 GitHub Releases 的最新版本；--update-atom 改用本地 atom 文件；
    // --download-to 顺便把该版本的 zip 下到这个目录（给自检用）。
    bool updateCheck = false;
    std::wstring updateAtomPath;
    std::wstring updateDownloadDir;
};

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
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

std::int64_t NowUnixSeconds() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const long long ticks = (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return (ticks - 116444736000000000LL) / 10000000LL;
}

std::string DayStamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", static_cast<int>(now.wYear),
                  static_cast<int>(now.wMonth), static_cast<int>(now.wDay));
    return buffer;
}

// 与 1.x 的日志同一格式："2026-11-08 13:05:00.123 [INFO] ..."。
std::string Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  static_cast<int>(now.wYear), static_cast<int>(now.wMonth),
                  static_cast<int>(now.wDay), static_cast<int>(now.wHour),
                  static_cast<int>(now.wMinute), static_cast<int>(now.wSecond),
                  static_cast<int>(now.wMilliseconds));
    return buffer;
}

double PrivateMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return -1.0;
    }
    // 与 1.x 的环境摘要同一口径：私有内存就是 PrivateUsage。
    return static_cast<double>(counters.PrivateUsage) / 1048576.0;
}

double WorkingSetMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.WorkingSetSize) / 1048576.0;
}

std::int64_t FileBytesUtf8(const std::string& utf8Path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(Utf8ToWide(utf8Path).c_str(), GetFileExInfoStandard, &data)) {
        return -1;
    }
    return (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) |
           static_cast<std::int64_t>(data.nFileSizeLow);
}

// ---- 报告：每行都是 KEY=value，与其它探针同一读法 ----

void AddText(std::string& report, const char* key, std::string_view value) {
    report += key;
    report += '=';
    report += value;
    report += '\n';
}

void AddInt(std::string& report, const char* key, long long value) {
    AddText(report, key, std::to_string(value));
}

void AddFlag(std::string& report, const char* key, bool value) {
    AddText(report, key, value ? "1" : "0");
}

void AddMb(std::string& report, const char* key, double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    AddText(report, key, buffer);
}

bool WriteReportFile(const std::wstring& path, const std::string& report) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::size_t written = 0;
    while (written < report.size()) {
        DWORD done = 0;
        if (!WriteFile(file, report.data() + written,
                       static_cast<DWORD>(report.size() - written), &done, nullptr) ||
            done == 0) {
            CloseHandle(file);
            return false;
        }
        written += done;
    }
    CloseHandle(file);
    return true;
}

// 日志：一行带时间戳的文本交给 IStore。写不进去不抛、不影响主流程（1.x 的 Log 同一条纪律）。
class AppLog {
public:
    explicit AppLog(core::IStore& store) : store_(store) {}

    void Info(std::string_view message) { Write("INFO", message); }
    void Error(std::string_view message) { Write("ERROR", message); }

    const std::string& LastFile() const { return lastFile_; }

private:
    void Write(const char* level, std::string_view message) {
        if (!pruned_) {
            // 清理只做一次：1.x 用 Interlocked 保证同一件事。
            core::PruneLogs(store_, NowUnixSeconds());
            pruned_ = true;
        }

        std::string line = Timestamp();
        line += " [";
        line += level;
        line += "] ";
        line.append(message);
        line += '\n';
        lastFile_ = core::AppendLogLine(store_, DayStamp(), line);
    }

    core::IStore& store_;
    bool pruned_ = false;
    std::string lastFile_;
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--selftest") {
            options.selftest = true;
        } else if (argument == "--seconds" && i + 1 < argc) {
            options.seconds = std::atoi(argv[++i]);
            if (options.seconds < 1) {
                options.seconds = 1;
            }
        } else if (argument == "--report" && i + 1 < argc) {
            options.reportPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--overlay-size" && i + 1 < argc) {
            int width = 0;
            int height = 0;
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
                options.overlayWidth = width;
                options.overlayHeight = height;
            }
        } else if (argument == "--record-window") {
            options.recordWindow = true;
        } else if (argument == "--record-region" && i + 1 < argc) {
            int width = 0;
            int height = 0;
            int x = 0;
            int y = 0;
            if (std::sscanf(argv[++i], "%dx%d@%d,%d", &width, &height, &x, &y) == 4 &&
                width >= 2 && height >= 2) {
                options.recordRegion = core::IntRect{ x, y, x + (width & ~1), y + (height & ~1) };
                options.hasRecordRegion = true;
            }
        } else if (argument == "--record-seconds" && i + 1 < argc) {
            options.recordSeconds = std::atoi(argv[++i]);
            if (options.recordSeconds < 0) {
                options.recordSeconds = 0;
            }
        } else if (argument == "--screenshot-selftest" && i + 1 < argc) {
            options.screenshotSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--ocr-selftest" && i + 1 < argc) {
            options.ocrSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--settings-selftest" && i + 1 < argc) {
            options.settingsSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--pin-selftest" && i + 1 < argc) {
            options.pinSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--radial-selftest" && i + 1 < argc) {
            options.radialSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--settings-click-selftest" && i + 1 < argc) {
            options.settingsClickSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--hotkey-selftest" && i + 1 < argc) {
            options.hotkeySelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--hotkey-live-selftest" && i + 1 < argc) {
            options.hotkeyLiveSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--ocr-search-selftest" && i + 1 < argc) {
            options.ocrSearchSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--ocr-model-download-selftest" && i + 1 < argc) {
            options.ocrModelDownloadSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--settings-page" && i + 1 < argc) {
            options.settingsSelftestPage = std::atoi(argv[++i]);
        } else if (argument == "--settings-scroll" && i + 1 < argc) {
            options.settingsSelftestScroll = std::atoi(argv[++i]);
        } else if (argument == "--radial-click-selftest" && i + 1 < argc) {
            options.radialClickSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--window-picker-selftest" && i + 1 < argc) {
            options.windowPickerSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--recording-ui-selftest" && i + 1 < argc) {
            options.recordingUiSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--tray-menu-selftest" && i + 1 < argc) {
            options.trayMenuSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--version") {
            options.version = true;
        } else if (argument == "--check-update" && i + 1 < argc) {
            options.updateManifestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--update-check") {
            options.updateCheck = true;
        } else if (argument == "--update-atom" && i + 1 < argc) {
            options.updateAtomPath = Utf8ToWide(argv[++i]);
            options.updateCheck = true;
        } else if (argument == "--download-to" && i + 1 < argc) {
            options.updateDownloadDir = Utf8ToWide(argv[++i]);
        } else if (argument == "--no-tray") {
            options.tray = false;
        } else if (argument == "--no-input") {
            options.input = false;
        } else if (argument == "--no-overlay") {
            options.overlay = false;
        } else if (argument == "--help" || argument == "-h") {
            options.help = true;
        }
    }
    return options;
}

void PrintUsage() {
    std::printf(
        "HaloCap (2.0 native skeleton)\n"
        "  --selftest [--seconds N] [--report PATH]\n"
        "  --record-seconds N    record N seconds, then exit (unattended check)\n"
        "  --screenshot-selftest PATH  open the screenshot overlay, annotate, export, exit\n"
        "  --ocr-selftest PATH         open the OCR overlay, recognise the selection, exit\n"
        "  --settings-selftest PATH    open the six settings pages, capture, exit\n"
        "  --hotkey-selftest PATH      inject keys through hotkey capture, capture, exit\n"
        "  --hotkey-live-selftest PATH change a hotkey through settings, press it, exit\n"
        "  --ocr-search-selftest PATH  drive the result window search on a synthetic result, exit\n"
        "  --pin-selftest PATH         pin the clipboard content, capture, exit\n"
        "  --radial-selftest PATH      open the radial menu, capture, exit\n"
        "  --window-picker-selftest PATH  open the window picker, capture, exit\n"
        "  --recording-ui-selftest PATH   drive frame/countdown/HUD, capture, exit\n"
        "  --tray-menu-selftest PATH      open the drawn tray menu, click, capture, exit\n"
        "  --version                   print the delivery version and exit\n"
        "  --check-update PATH         compare this build with an update manifest\n"
        "  --update-check               query GitHub Releases for the latest version\n"
        "  --update-atom PATH           use a local releases.atom instead of GitHub\n"
        "  --download-to DIR            with --update-check: also download the zip\n"
        "  --settings-selftest PATH    open the settings pages, capture, exit\n"
        "  --overlay-size 1920x1080\n"
        "  --no-tray | --no-input | --no-overlay\n");
}

// 覆盖层：按需创建。第一次截图才会用，所以 idle 的时候它不该存在。
class OverlayStack {
public:
    bool Initialize(AppLog& log, int width, int height) {
        if (ready_) {
            return true;
        }

        if (!backend_.Initialize()) {
            log.Error("overlay: D3D/D2D backend failed, hr=0x" + Hex(backend_.LastHr()));
            return false;
        }

        if (!rasterizer_.Initialize()) {
            log.Error("overlay: font rasterizer failed, hr=0x" + Hex(rasterizer_.LastHr()));
            return false;
        }

        // 覆盖层上总有一行提示文字（1.x 的「拖动选择区域」那一类）。这里也排版一次，
        // 否则量到的私有内存会少掉一页 1024×1024 图集（4 MB）——那正是 spike 3 的
        // 60 MB 地板里含着的东西，省掉它会让 S1 的数字偏乐观。
        const core::FontSpec hintSpec{ "Segoe UI", 400, 24.0f,
                                       core::Color{ 1.0f, 1.0f, 1.0f, 1.0f } };
        hint_ = rasterizer_.LayOut("HaloCap · 选区", hintSpec, core::Point{ 40.0f, 40.0f });

        const core::WindowBounds bounds{ -32000, -32000, width, height };
        const core::WindowOptions options{ true, false, true, true };
        if (!window_.Create(bounds, options)) {
            log.Error("overlay: window create failed, lastError=" +
                      std::to_string(GetLastError()));
            return false;
        }

        if (!backend_.AttachWindow(window_.Handle(), width, height)) {
            log.Error("overlay: attach failed, hr=0x" + Hex(backend_.LastHr()));
            return false;
        }

        ready_ = true;
        return true;
    }

    bool Ready() const { return ready_; }

    // 录制要复用同一台 D3D 设备：采集、合成、编码必须是同一个设备/适配器（计划 §4）。
    D2dRenderBackend& Backend() { return backend_; }

private:
    static std::string Hex(long value) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(value));
        return buffer;
    }

    D2dRenderBackend backend_;
    D2dFontRasterizer rasterizer_{ backend_ };
    Win32SurfaceWindow window_;
    core::TextCell hint_{};
    bool ready_ = false;
};

// ---- 录制：托盘「录屏」与录制热键的落点 ----

std::int64_t QpcNowUs() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::int64_t>(now.QuadPart) * 1000000 / frequency.QuadPart;
}

// 1.x 的规则：设置里给了就用它（展开 %USERPROFILE% 这类变量），否则放到「视频\Captures」。
std::wstring RecordingDirectory(const core::AppSettings& settings) {
    if (!settings.recordingDirectory.empty()) {
        const std::wstring configured = Utf8ToWide(settings.recordingDirectory);
        wchar_t expanded[MAX_PATH] = {};
        if (ExpandEnvironmentStringsW(configured.c_str(), expanded, MAX_PATH) > 0) {
            return expanded;
        }
        return configured;
    }

    // 系统的「视频」已知文件夹（用户可能把它重定向到别的盘），下面挂 Captures。
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videos)) &&
        videos != nullptr) {
        std::wstring directory = std::wstring(videos) + L"\\Captures";
        CoTaskMemFree(videos);
        return directory;
    }

    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) {
        return {};
    }
    return std::wstring(profile) + L"\\Videos\\Captures";
}

// 在资源管理器里定位文件；文件不在了就退回到它的目录。
void RevealInExplorer(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    std::wstring arguments;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        arguments = L"/select,\"" + path + L"\"";
    } else {
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (parent.empty()) {
            return;
        }
        arguments = L"\"" + parent.wstring() + L"\"";
    }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring RecordingPathFor(const core::AppSettings& settings) {
    const std::wstring directory = RecordingDirectory(settings);
    if (directory.empty()) {
        return {};
    }

    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(directory), ignored);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[160] = {};
    std::swprintf(name, 160, L"HaloCap_%04d-%02d-%02d_%02d-%02d-%02d.mp4",
                  static_cast<int>(now.wYear), static_cast<int>(now.wMonth),
                  static_cast<int>(now.wDay), static_cast<int>(now.wHour),
                  static_cast<int>(now.wMinute), static_cast<int>(now.wSecond));
    std::wstring path = directory + L"\\" + name;

    // 同名加序号（1.x 的 AppPaths 也是这么做的）。
    int index = 2;
    while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::swprintf(name, 160, L"HaloCap_%04d-%02d-%02d_%02d-%02d-%02d_%d.mp4",
                      static_cast<int>(now.wYear), static_cast<int>(now.wMonth),
                      static_cast<int>(now.wDay), static_cast<int>(now.wHour),
                      static_cast<int>(now.wMinute), static_cast<int>(now.wSecond), index);
        path = directory + L"\\" + name;
        ++index;
    }
    return path;
}

// 录制的全部对象都在这里：采集、编码、两路音频与录制会话。Start 时建、Stop 时拆，
// 所以 idle 的时候它们一个字节都不占（S1 的 80 MB 预算靠这一条守住）。
class RecordingRunner {
public:
    bool Start(D2dRenderBackend& backend, const core::AppSettings& settings, AppLog& log,
               std::string& errorKind, core::NativeHandle captureWindow = nullptr,
               core::IntRect cropRegion = {}, bool captureMonitor = false,
               bool captureCursor = true) {
        errorKind.clear();

        const std::wstring path = RecordingPathFor(settings);
        if (path.empty()) {
            errorKind = "path";
            return false;
        }
        path_ = WideToUtf8(path);

        log.Info("recording: capture initializing");
        if (captureWindow != nullptr || captureMonitor) {
            capture_ = std::make_unique<Win32WgcCapture>(backend);
            capture_->SetCursorCapture(captureCursor);
        } else {
            auto duplication = std::make_unique<Win32DesktopDuplication>(backend);
            if (!duplication->Initialize()) {
                errorKind = "capture";
                Reset();
                return false;
            }
            capture_ = std::move(duplication);
        }

        log.Info("recording: encoder initializing");
        encoder_ = std::make_unique<MfH264Encoder>(backend);
        if (!encoder_->Initialize()) {
            errorKind = "encoder";
            Reset();
            return false;
        }

        if (settings.recording.systemAudio) {
            systemAudio_ =
                std::make_unique<WasapiAudioSource>(core::AudioSourceKind::SystemLoopback);
        }
        if (settings.recording.microphone) {
            microphone_ = std::make_unique<WasapiAudioSource>(core::AudioSourceKind::Microphone);
        }

        log.Info(std::string("recording: session starting audio=") + (settings.recording.WantsAudio() ? "1" : "0"));
        session_ = std::make_unique<core::RecordingSession>(*capture_, *encoder_,
                                                            systemAudio_.get(), microphone_.get());

        core::RecordingOptions options;
        // 显示器录制：选区在哪个显示器上就采哪个，不能固定采 0 号（多屏时副屏选区会被
        // 当成主屏坐标而裁错/退化成整屏）。窗口录制不需要输出序号。
        options.outputIndex =
            (captureMonitor && captureWindow == nullptr && !cropRegion.IsEmpty())
                ? halocap::platform::win32::WgcMonitorIndexForRect(cropRegion)
                : 0;
        options.targetFps = settings.recording.FramesPerSecond();
        options.outputPathUtf8 = path_;
        options.audioEnabled = settings.recording.WantsAudio();
        options.captureWindow = captureWindow;
        options.cropRegion = cropRegion;
        options.captureMonitor = captureMonitor && captureWindow == nullptr;

        if (!session_->Start(options, QpcNowUs())) {
            errorKind = session_->LastErrorKind();
            Reset();
            return false;
        }
        return true;
    }

    bool Pump(std::int64_t wallUs) { return session_ != nullptr && session_->Pump(wallUs); }

    bool Pause(std::int64_t wallUs) { return session_ != nullptr && session_->Pause(wallUs); }

    bool Resume(std::int64_t wallUs) { return session_ != nullptr && session_->Resume(wallUs); }

    std::int64_t MediaTimeMs() const {
        return session_ != nullptr ? session_->Stats().mediaTimeUs / 1000 : 0;
    }

    bool Paused() const { return session_ != nullptr && session_->Paused(); }

    bool Stop(core::RecordingStats& stats) {
        if (session_ == nullptr) {
            stats = core::RecordingStats{};
            return false;
        }
        const bool ok = session_->Stop(QpcNowUs(), stats);
        Reset();
        return ok;
    }

    bool Active() const { return session_ != nullptr; }

    const char* LastErrorKind() const {
        return session_ != nullptr ? session_->LastErrorKind() : "none";
    }

    const std::string& OutputPath() const { return path_; }

    // 浮层信息行与电平表要的那几个数，原样转给会话（1.x 的 RecordingHudState 同源）。
    int OutputWidth() const { return session_ != nullptr ? session_->OutputWidth() : 0; }
    int OutputHeight() const { return session_ != nullptr ? session_->OutputHeight() : 0; }
    int TargetFps() const { return session_ != nullptr ? session_->TargetFps() : 0; }
    bool SystemAudioActive() const {
        return session_ != nullptr && session_->SystemAudioActive();
    }
    bool MicrophoneActive() const {
        return session_ != nullptr && session_->MicrophoneActive();
    }
    float SystemAudioLevel() const {
        return session_ != nullptr ? session_->SystemAudioLevel() : 0.0f;
    }
    float MicrophoneLevel() const {
        return session_ != nullptr ? session_->MicrophoneLevel() : 0.0f;
    }

private:
    void Reset() {
        session_.reset();
        capture_.reset();
        encoder_.reset();
        systemAudio_.reset();
        microphone_.reset();
    }

    std::unique_ptr<core::ICaptureSource> capture_;
    std::unique_ptr<MfH264Encoder> encoder_;
    std::unique_ptr<WasapiAudioSource> systemAudio_;
    std::unique_ptr<WasapiAudioSource> microphone_;
    std::unique_ptr<core::RecordingSession> session_;
    std::string path_;
};

// 1.x 的 FormatSize："0.##" —— 最多两位小数，末尾的零去掉（12.50 → "12.5"，12.00 → "12"）。
std::string FormatRecordingSize(std::int64_t bytes) {
    const bool gigabytes = bytes >= 1024LL * 1024 * 1024;
    const double value = gigabytes ? bytes / 1073741824.0 : bytes / 1048576.0;
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text + (gigabytes ? " GB" : " MB");
}

// 释放 ORT 会话之后把空出来的堆页还给系统：PrivateUsage 是「已提交」，光 free 不减。
void TrimProcessMemory() {
    _heapmin();
    if (HANDLE heap = GetProcessHeap()) {
        HeapCompact(heap, 0);
    }
}

std::string ReadTextFileUtf8(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

std::string ResolveOcrModelDirectory() {    if (const char* environment = std::getenv("HALOCAP_OCR_MODELS")) {
        return environment;
    }

    std::vector<std::filesystem::path> candidates;
    wchar_t executable[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0) {
        const std::filesystem::path directory = std::filesystem::path(executable).parent_path();
        candidates.push_back(directory / "models" / "ppocrv6-tiny-1");
        candidates.push_back(directory / ".." / ".." / ".." / "src" / "HaloCap" / "Assets" /
                             "OcrModel");
    }
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0) {
        candidates.push_back(std::filesystem::path(localAppData) / "HaloCap2" / "models" /
                             "ppocrv6-tiny-1");
    }

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate / "ppocrv6_tiny_det.onnx", error)) {
            return candidate.string();
        }
    }
    return {};
}

std::vector<std::string> ListModelFiles(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error)) {
            names.push_back(WideToUtf8(entry.path().filename().wstring()));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// 按设置里选的模型目录名解析：先找用户模型目录，再找程序目录；都没有就用内置模型。
std::string ResolveOcrModelDirectoryFor(const std::string& modelId) {
    if (!modelId.empty()) {
        std::vector<std::filesystem::path> roots;
        wchar_t executable[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0) {
            roots.push_back(std::filesystem::path(executable).parent_path() / "models");
        }
        wchar_t localAppData[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0) {
            roots.push_back(std::filesystem::path(localAppData) / "HaloCap2" / "models");
        }
        for (const std::filesystem::path& root : roots) {
            const std::filesystem::path candidate = root / modelId;
            std::error_code error;
            if (!std::filesystem::is_directory(candidate, error)) {
                continue;
            }
            if (core::DetectOcrModelFiles(ListModelFiles(candidate)).Complete()) {
                return candidate.string();
            }
        }
    }
    return ResolveOcrModelDirectory();
}

int Run(const Options& options) {
    // DPI 必须在枚举输出与建窗口之前设好，否则坐标按 DIP 算（1.x 与采集 seam 的老坑）。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::string report;
    Win32Capability capability;
    Win32Store store;
    AppLog log(store);

    const core::PlatformFacts facts = capability.Probe();
    const core::FeatureFlags flags = core::EvaluateFeatures(facts);

    log.Info("starting: build=" + std::to_string(facts.osBuild) +
             " wgc=" + (facts.graphicsCaptureSupported ? "1" : "0") +
             " recording=" + (flags.recording ? "1" : "0"));

    // Media Foundation 一次启动，进程内复用（编码器热启动的成本因此只在第一次）。
    MfH264Encoder::StartupFoundation();

    // ---- 1) 设置：读自己的目录；首次启动写一份默认值 ----
    const core::SettingsLoadResult loaded = core::LoadSettingsFrom(store);
    bool settingsSaved = false;
    if (!loaded.fileExists) {
        settingsSaved = core::SaveSettingsTo(store, loaded.settings);
        log.Info(settingsSaved ? "settings: first run, defaults written"
                               : std::string("settings: first run, defaults not written (") +
                                     store.LastErrorKind() + ")");
    } else if (!loaded.report.parseError.empty()) {
        // 坏文件已经退回默认值，这里只记原因（1.x 在这条路上只写日志、不告诉用户）。
        log.Error("settings: parse error '" + loaded.report.parseError + "', using defaults");
    } else {
        log.Info("settings: loaded");
    }

    core::AppSettings liveSettings = loaded.settings;
    // 保存目录回显：没设过就用系统「视频\Captures」（自动获取），写进设置让设置页显示真实
    // 路径、并在下次保存时落盘；用户仍可在设置页改到别处。
    if (liveSettings.recordingDirectory.empty()) {
        liveSettings.recordingDirectory = WideToUtf8(RecordingDirectory(liveSettings));
    }
    const core::AppSettings& settings = liveSettings;

    AddFlag(report, "APP_SETTINGS_FILE_EXISTS", loaded.fileExists);
    AddFlag(report, "APP_SETTINGS_SAVED", settingsSaved);
    AddInt(report, "APP_SETTINGS_MIGRATED", static_cast<long long>(loaded.report.migratedKeys.size()));
    AddInt(report, "APP_SETTINGS_IGNORED", static_cast<long long>(loaded.report.ignoredKeys.size()));
    AddInt(report, "APP_SETTINGS_REPAIRED", static_cast<long long>(loaded.report.repairedKeys.size()));
    AddInt(report, "APP_SETTINGS_DROPPED", static_cast<long long>(loaded.report.droppedKeys.size()));
    AddText(report, "APP_RECORD_DIRECTORY", liveSettings.recordingDirectory);
    AddText(report, "APP_OCR_MODEL", liveSettings.ocr.model);
    AddText(report, "APP_OCR_MODEL_DIR",
            ResolveOcrModelDirectoryFor(liveSettings.ocr.model));

    // ---- 2) 托盘与热键 ----
    core::TrayMenuState trayState;
    trayState.radialHotkey = core::FormatHotkeyText(settings.radialHotkey);
    trayState.screenshotHotkey = core::FormatHotkeyText(settings.screenshotHotkey);
    trayState.ocrHotkey = core::FormatHotkeyText(settings.ocrHotkey);
    trayState.recordHotkey = core::FormatHotkeyText(settings.recordHotkey);
    trayState.pinHotkey = core::FormatHotkeyText(settings.pinHotkey);
    // 贴图已经接上；开机自启服务还没接（S4），先按「未开启」呈现。
    trayState.pinAvailable = true;
    wchar_t executablePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    const std::wstring selfPath = executablePath;
    if (settings.autoStart != halocap::platform::win32::IsAutoStartEnabled(selfPath)) {
        std::string autoStartError;
        if (!halocap::platform::win32::SetAutoStart(settings.autoStart, selfPath, &autoStartError)) {
            log.Error("autostart: apply failed (" + autoStartError + ")");
        }
    }
    trayState.autoStart = halocap::platform::win32::IsAutoStartEnabled(selfPath);

    Win32Tray tray;
    bool trayIcon = false;
    if (options.tray) {
        trayIcon = tray.Start();
        if (trayIcon) {
            tray.Apply(trayState);
            log.Info("tray: icon added");
        } else {
            log.Error(std::string("tray: icon unavailable (") + tray.LastErrorKind() + ")");
        }
    } else {
        log.Info("tray: disabled by --no-tray");
    }

    Win32InputHost input;
    int hotkeysRegistered = 0;
    bool inputStarted = false;

    // 启动与「设置保存」共用这一条注册路：先全部摘掉再按槽位序注册，任何一把失败
    // 都由调用方决定是否退回原样。error 非空时带回第一把失败的可读原因。
    const auto registerAllHotkeys = [&](const core::AppSettings& candidate, std::wstring* error) {
        for (int token : kHotkeyTokens) {
            input.UnregisterHotkey(token);
        }
        int registered = 0;
        for (int slot = 0; slot < core::kHotkeySlotCount; ++slot) {
            const core::HotkeySpec& spec = *core::HotkeySlot(candidate, slot);
            if (input.RegisterHotkey(spec, kHotkeyTokens[slot])) {
                ++registered;
                log.Info(std::string("hotkey: ") + kHotkeyNames[slot] + " = " +
                         core::FormatHotkeyText(spec));
                continue;
            }
            log.Error(std::string("hotkey: ") + kHotkeyNames[slot] + " unavailable (" +
                      input.LastErrorKind() + ")");
            if (error != nullptr && error->empty()) {
                const std::wstring display = Utf8ToWide(core::FormatHotkeyText(spec));
                *error = std::wstring(L"「") + kHotkeyLabels[slot] + L"」：";
                if (std::strcmp(input.LastErrorKind(), "inuse") == 0) {
                    *error += L"「" + display + L"」已被其他程序占用，换一个组合键试试。";
                } else {
                    *error += L"无法注册快捷键「" + display + L"」（错误码 " +
                              std::to_wstring(input.LastErrorCode()) + L"）。";
                }
            }
        }
        // 单键热键必须独占该键：不拦截的话前台程序还会再收到一次同一个键，
        // 设置页那句「单键会独占该键并优先于输入法」才算数。
        std::vector<core::HotkeySpec> singleKeys;
        for (int slot = 0; slot < core::kHotkeySlotCount; ++slot) {
            const core::HotkeySpec& spec = *core::HotkeySlot(candidate, slot);
            if ((spec.modifiers & core::kStateModifiers) == 0) {
                singleKeys.push_back(spec);
            }
        }
        input.SetInterception(singleKeys);
        return registered;
    };

    if (options.input) {
        inputStarted = input.Start();
        if (inputStarted) {
            hotkeysRegistered = registerAllHotkeys(settings, nullptr);
        } else {
            log.Error(std::string("input: host failed to start (") + input.LastErrorKind() + ")");
        }
    } else {
        log.Info("input: disabled by --no-input");
    }

    AddFlag(report, "APP_TRAY_ICON", trayIcon);

    // 气泡点击通路：给托盘窗口投一条 NIN_BALLOONUSERCLICK（WM_APP+1 是托盘回调消息，
    // wParam 1 是图标 id），PollBalloonClick 必须拿到，否则点「已保存」不会有反应。
    bool balloonClick = false;
    if (trayIcon && tray.Window() != nullptr) {
        SendMessageW(static_cast<HWND>(tray.Window()), WM_APP + 1, 1,
                     static_cast<LPARAM>(NIN_BALLOONUSERCLICK));
        balloonClick = tray.PollBalloonClick();
    }
    AddFlag(report, "APP_TRAY_BALLOON_CLICK", balloonClick);
    AddFlag(report, "APP_INPUT_HOST", inputStarted);
    AddInt(report, "APP_HOTKEYS_REGISTERED", hotkeysRegistered);

    // ---- 3) 覆盖层：按需 ----
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int overlayWidth = options.overlayWidth > 0 ? options.overlayWidth : (screenWidth > 0 ? screenWidth : 1920);
    const int overlayHeight =
        options.overlayHeight > 0 ? options.overlayHeight : (screenHeight > 0 ? screenHeight : 1080);

    AddInt(report, "APP_SCREEN_WIDTH", screenWidth > 0 ? screenWidth : 0);
    AddInt(report, "APP_SCREEN_HEIGHT", screenHeight > 0 ? screenHeight : 0);
    AddInt(report, "APP_OVERLAY_WIDTH", overlayWidth);
    AddInt(report, "APP_OVERLAY_HEIGHT", overlayHeight);

    AddText(report, "APP_OS_BUILD", std::to_string(facts.osBuild));
    AddFlag(report, "APP_WGC", facts.graphicsCaptureSupported);
    AddFlag(report, "APP_RECORDING_ALLOWED", flags.recording);
    AddText(report, "APP_FLOOR_MESSAGE",
            core::DescribeWindowsFloor(facts.osBuild).empty() ? "none"
                                                              : core::DescribeWindowsFloor(facts.osBuild));
    AddText(report, "APP_LOG_FILE", log.LastFile().empty() ? "(blocked)" : log.LastFile());
    AddText(report, "APP_SETTINGS_PATH", store.SettingsPath());

    // ---- 4) 消息循环 ----
    // 阶段：0 = 空闲（没有覆盖层），1 = 覆盖层挂上之后。selftest 在每段末尾采样一次内存。
    const std::int64_t startMs = static_cast<std::int64_t>(GetTickCount64());
    const std::int64_t idleDeadlineMs = 1500;
    const std::int64_t overlaySettleMs = 2000;
    const std::int64_t hardDeadlineMs = 30000;

    OverlayStack overlay;
    bool running = true;
    bool sampledIdle = false;
    // 启动不预热：这里的 OverlayStack 只服务录制（beginRecording 里按需建），
    // 截图/OCR 走的是 ScreenshotOverlay 自己的设备——预热从不服务它们，只是把
    // 58 MB 常驻拖到进程余生（实测：预热时 OCR 后 99 MB，不预热 41 MB）。
    bool overlayReady = false;
    std::int64_t overlayAtMs = 0;
    double idlePrivate = -1.0;
    double idleWorkingSet = -1.0;
    double overlayPrivate = -1.0;
    double overlayWorkingSet = -1.0;
    int actions = 0;

    // ---- 录制入口：托盘「录屏」与录制热键共用这一个开关 ----
    RecordingRunner recording;
    bool recordingActive = false;
    std::unique_ptr<halocap::app::RecordingHud> recordingHud;
    std::unique_ptr<halocap::app::RecordingFrame> recordingFrame;
    // 窗口录制：目标窗口与它当前的可见矩形。与 1.x 一样每 250ms 跟随一次，
    // 关闭/最小化就收尾保存，挪动/改尺寸就更新边框与浮层。
    HWND recordTargetWindow = nullptr;
    core::IntRect recordAnchor{};
    std::int64_t lastHudInfoMs = 0;
    std::wstring hudInfoText;
    std::unique_ptr<halocap::app::WindowPicker> windowPicker;
    std::unique_ptr<halocap::app::ScreenshotOverlay> screenshotOverlay;
    std::vector<std::unique_ptr<halocap::app::PinWindow>> pins;
    bool recordStarted = false;
    bool recordStopped = false;
    // 录制实际输出的宽高（验证多屏选区裁剪是否落在选中的那块）。停止后会话即释放，
    // 所以在 Stop 之前先记下来。
    int recordOutputWidth = 0;
    int recordOutputHeight = 0;
    core::RecordingStats recordStats;

    auto refreshTray = [&]() {
        trayState.recording = recordingActive;
        trayState.recordingPaused = recordingActive && recording.Paused();
        if (trayIcon) {
            tray.Apply(trayState);
        }
    };

    // 气泡通知：只有「已保存」那条可点击跳转（点气泡 = 在资源管理器里选中文件）；
    // 其它通知一出现就把记住的路径清掉，免得点到旧气泡却打开上一次的文件。
    std::string savedBalloonPath;
    const auto notify = [&](std::string_view message) {
        savedBalloonPath.clear();
        tray.Notify("HaloCap", message);
    };
    const auto notifySaved = [&](const std::string& path) {
        savedBalloonPath = path;
        const std::string message = "已保存：" + path;
        tray.Notify("HaloCap", message);
    };

    // 1.x WindowGeometry.TryGet 的同款判据：活着、没最小化、可见、表面 ≥16×16、可见矩形非空。
    const auto recordWindowUsable = [](HWND window, RECT* visible) {
        if (window == nullptr || !IsWindow(window) || IsIconic(window) ||
            !IsWindowVisible(window)) {
            return false;
        }
        RECT surface{};
        if (!GetWindowRect(window, &surface) || surface.right - surface.left < 16 ||
            surface.bottom - surface.top < 16) {
            return false;
        }
        if (!halocap::app::WindowVisibleRect(window, visible)) {
            return false;
        }
        return visible->right > visible->left && visible->bottom > visible->top;
    };

    // 每一条停止路（按钮、热键、采集丢失、窗口关闭、限时）都从这里收掉边框与浮层。
    const auto closeRecordingUi = [&]() {
        if (recordingHud != nullptr) {
            recordingHud->Close();
            recordingHud.reset();
        }
        if (recordingFrame != nullptr) {
            recordingFrame->Close();
            recordingFrame.reset();
        }
        recordTargetWindow = nullptr;
        recordAnchor = {};
        lastHudInfoMs = 0;
        hudInfoText.clear();
    };

    // 信息行与 1.x BuildHudInfo 同源：尺寸·帧率·文件大小（文件还没建就不显示）·音轨标签。
    const auto buildHudInfo = [&]() {
        std::wstring info = std::to_wstring(recording.OutputWidth()) + L"×" +
                            std::to_wstring(recording.OutputHeight()) + L" · " +
                            std::to_wstring(recording.TargetFps()) + L"fps";
        const std::int64_t bytes = FileBytesUtf8(recording.OutputPath());
        if (bytes >= 0) {
            info += L" · " + Utf8ToWide(FormatRecordingSize(bytes));
        }
        if (settings.recording.systemAudio || settings.recording.microphone) {
            const bool system = recording.SystemAudioActive();
            const bool microphone = recording.MicrophoneActive();
            if (!system && !microphone) {
                info += L" · 无音轨";
            } else if (system && microphone) {
                info += L" · 系统+麦";
            } else if (system) {
                info += L" · 系统声音";
            } else {
                info += L" · 麦克风";
            }
        }
        return info;
    };

    const auto beginRecording = [&](core::NativeHandle captureWindow, core::IntRect cropRegion) {
        HWND target = static_cast<HWND>(captureWindow);
        core::IntRect anchor{};
        if (target != nullptr) {
            RECT visible{};
            if (!recordWindowUsable(target, &visible)) {
                notify("目标窗口已关闭或尺寸太小，请重新选择窗口。");
                return;
            }
            anchor = core::IntRect{ visible.left, visible.top, visible.right, visible.bottom };
        } else if (!cropRegion.IsEmpty()) {
            anchor = cropRegion;
        } else {
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY),
                            &monitorInfo);
            anchor = core::IntRect{ monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                                    monitorInfo.rcMonitor.right, monitorInfo.rcMonitor.bottom };
        }

        // 1.x 的顺序：边框先挂出来再倒计时，倒计时里用户能确认录的就是这一块。
        recordingFrame = std::make_unique<halocap::app::RecordingFrame>();
        if (!recordingFrame->Show(anchor)) {
            recordingFrame.reset();
        }

        if (settings.recording.countdown != core::RecordingCountdown::Off) {
            halocap::app::RecordingCountdown countdown;
            if (!countdown.Show(settings.recording.CountdownSeconds(), anchor)) {
                log.Info("recording: cancelled during countdown");
                closeRecordingUi();
                notify("已取消录制");
                return;
            }
            // 倒计时期间目标窗口可能已经关闭或挪走。
            if (target != nullptr) {
                RECT visible{};
                if (!recordWindowUsable(target, &visible)) {
                    closeRecordingUi();
                    notify("倒计时期间目标窗口已关闭。");
                    return;
                }
                anchor = core::IntRect{ visible.left, visible.top, visible.right,
                                        visible.bottom };
                if (recordingFrame != nullptr) {
                    recordingFrame->UpdateRect(anchor);
                }
            }
        }

        if (!overlayReady) {
            // 录制要复用覆盖层那台 D3D 设备（采集/编码必须同一个设备）。
            overlayReady = overlay.Initialize(log, overlayWidth, overlayHeight);
        }
        std::string kind;
        const bool started = overlayReady &&
                             recording.Start(overlay.Backend(), settings, log, kind, captureWindow, cropRegion,
                                             captureWindow == nullptr,
                                             settings.recording.recordCursor);
        if (started) {
            recordingActive = true;
            recordTargetWindow = target;
            recordAnchor = anchor;
            lastHudInfoMs = 0;
            refreshTray();
            recordingHud = std::make_unique<halocap::app::RecordingHud>();
            if (!recordingHud->Create(anchor)) {
                recordingHud.reset();
            }
            log.Info("recording: started -> " + recording.OutputPath());
            notify("开始录制");
        } else {
            closeRecordingUi();
            log.Error("recording: start failed (" +
                      (overlayReady ? kind : std::string("overlay")) + ")");
            notify("无法开始录制");
        }
    };

    auto toggleRecording = [&]() {
        if (!recordingActive) {
            if (settings.recording.range == core::RecordingRange::Window) {
                if (windowPicker != nullptr && windowPicker->Active()) {
                    return;
                }
                windowPicker = std::make_unique<halocap::app::WindowPicker>();
                if (!windowPicker->Open([&](HWND target) {
                        beginRecording(target, core::IntRect{});
                    })) {
                    windowPicker.reset();
                    notify("无法选择录制窗口");
                }
                return;
            }
            if (settings.recording.range == core::RecordingRange::Region) {
                if (screenshotOverlay != nullptr && screenshotOverlay->Active()) {
                    return;
                }
                screenshotOverlay = std::make_unique<halocap::app::ScreenshotOverlay>();
                halocap::app::ScreenshotOverlayOptions overlayOptions;
                overlayOptions.mode = halocap::app::OverlayMode::RecordRegion;
                overlayOptions.autoStart = settings.recording.autoStart;
                overlayOptions.onRegion = [&](core::IntRect rect) {
                    beginRecording(nullptr, rect);
                };
                if (!screenshotOverlay->Open(overlayOptions)) {
                    screenshotOverlay.reset();
                    notify("无法选择录制区域");
                }
                return;
            }
            beginRecording(nullptr, core::IntRect{});
            return;
        }

        core::RecordingStats stats;
        const bool stopped = recording.Stop(stats);
        recordingActive = false;
        closeRecordingUi();
        refreshTray();
        log.Info(stopped ? "recording: stopped" : "recording: stop failed");
        if (stopped) {
            notifySaved(recording.OutputPath());
        } else {
            notify("录制失败，详见日志");
        }
    };

    auto togglePause = [&]() {
        if (!recordingActive) {
            return;
        }
        if (recording.Paused()) {
            recording.Resume(QpcNowUs());
        } else {
            recording.Pause(QpcNowUs());
        }
        refreshTray();
        log.Info(recording.Paused() ? "recording: paused" : "recording: resumed");
    };

    // --record-seconds：无人值守的端到端自检（不经过托盘与热键）。
    const bool recordRequested = options.recordSeconds > 0;
    std::int64_t recordDeadlineMs = 0;
    if (recordRequested) {
        HWND target = options.recordWindow ? GetForegroundWindow() : nullptr;
        core::IntRect anchor{};
        if (target != nullptr) {
            RECT visible{};
            if (!recordWindowUsable(target, &visible)) {
                target = nullptr;
            } else {
                anchor = core::IntRect{ visible.left, visible.top, visible.right,
                                        visible.bottom };
            }
        }
        if (target == nullptr && options.hasRecordRegion) {
            anchor = options.recordRegion;
        }
        if (anchor.IsEmpty()) {
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY),
                            &monitorInfo);
            anchor = core::IntRect{ monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                                    monitorInfo.rcMonitor.right, monitorInfo.rcMonitor.bottom };
        }
        recordingFrame = std::make_unique<halocap::app::RecordingFrame>();
        if (!recordingFrame->Show(anchor)) {
            recordingFrame.reset();
        }
        if (!overlayReady) {
            overlayReady = overlay.Initialize(log, overlayWidth, overlayHeight);
        }
        std::string kind;
        recordStarted = overlayReady &&
                        recording.Start(overlay.Backend(), settings, log, kind, target,
                                        options.hasRecordRegion ? options.recordRegion
                                                                : core::IntRect{},
                                        target == nullptr, settings.recording.recordCursor);
        if (recordStarted) {
            recordingActive = true;
            recordTargetWindow = target;
            recordAnchor = anchor;
            lastHudInfoMs = 0;
            refreshTray();
            recordingHud = std::make_unique<halocap::app::RecordingHud>();
            if (!recordingHud->Create(anchor)) {
                recordingHud.reset();
            }
            recordDeadlineMs =
                static_cast<std::int64_t>(GetTickCount64()) + options.recordSeconds * 1000;
            log.Info("recording: started (--record-seconds) -> " + recording.OutputPath());
        } else {
            closeRecordingUi();
            log.Error("recording: start failed (" +
                      (overlayReady ? kind : std::string("overlay")) + ")");
            running = false;
        }
    }


    // OCR：推理放在一次性的 worker 子进程里，主进程不加载 ORT。
    // 结果窗口和 1.x 一样可以同时开多个，不因为下一次识别或截图自动关掉。
    std::vector<std::unique_ptr<halocap::app::OcrResultWindow>> ocrWindows;
    std::unique_ptr<halocap::app::SettingsWindow> settingsWindow;
    int settingsPageOverride = options.settingsSelftestPage;
    std::unique_ptr<halocap::app::RadialMenu> radialMenu;
    std::unique_ptr<halocap::app::TrayMenuWindow> trayMenu;
    // 贴图窗口的公共接线：右键菜单里的 OCR / 全部关闭 / 外描边都要落到 app 这一层。
    const auto pinOptions = [&]() {
        halocap::app::PinWindowOptions options;
        options.outline = liveSettings.pinGlowEnabled;
        options.ocrEnabled = true;
        options.jpeg = settings.screenshotFormat == core::ScreenshotFormat::Jpeg;
        return options;
    };

    // OCR 识别在后台线程跑 worker，完成后 PostMessage 回结果窗口。结果窗口一打开就显示
    // 「正在识别」，切换引擎/重试走的都是这条路径。
    const auto startOcr = [&liveSettings](core::OcrEngine engine, int token,
                                          const std::uint8_t* bgra, int width, int height,
                                          HWND notify) {
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            bgra, bgra + static_cast<std::size_t>(width) * height * 4);
        const std::string engineName = engine == core::OcrEngine::Native ? "winrt" : "pp";
        std::string models = ResolveOcrModelDirectoryFor(liveSettings.ocr.model);
        const std::string clsDirectory = ResolveOcrModelDirectory();
        std::thread([pixels, width, height, engine, token, notify, engineName, models,
                     clsDirectory]() mutable {
            auto* outcome = new halocap::app::OcrOutcome();
            outcome->token = token;
            outcome->engine = engine;
            wchar_t executable[MAX_PATH] = {};
            wchar_t temporary[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0 ||
                GetTempPathW(MAX_PATH, temporary) == 0) {
                outcome->error = "无法定位程序或临时目录";
            } else {
                const std::filesystem::path directory =
                    std::filesystem::path(temporary) / "HaloCap2";
                std::error_code directoryError;
                std::filesystem::create_directories(directory, directoryError);
                const std::wstring stem = L"ocr-" + std::to_wstring(GetCurrentProcessId()) +
                                          L"-" + std::to_wstring(GetTickCount64());
                const std::wstring inputPath = (directory / (stem + L".raw")).wstring();
                const std::wstring outputPath = (directory / (stem + L".txt")).wstring();
                const std::string inputUtf8 = WideToUtf8(inputPath);
                const std::string outputUtf8 = WideToUtf8(outputPath);
                if (!models.empty()) {
                    SetEnvironmentVariableW(L"HALOCAP_OCR_MODELS",
                                            Utf8ToWide(models).c_str());
                }
                if (!clsDirectory.empty()) {
                    SetEnvironmentVariableW(L"HALOCAP_OCR_CLS_DIR",
                                            Utf8ToWide(clsDirectory).c_str());
                }
                if (!halocap::app::WriteOcrWorkerInput(inputUtf8, pixels->data(), width,
                                                       height)) {
                    outcome->error = "无法写入识别输入";
                } else {
                    std::string error;
                    const std::int64_t start = QpcNowUs();
                    const bool spawned = halocap::app::SpawnOcrWorker(
                        WideToUtf8(executable), inputUtf8, outputUtf8, engineName, &error);
                    halocap::app::OcrWorkerResult worker;
                    const bool parsed =
                        spawned && halocap::app::ReadOcrWorkerOutput(outputUtf8, &worker);
                    outcome->elapsedMs =
                        static_cast<double>(QpcNowUs() - start) / 1000.0;
                    DeleteFileW(inputPath.c_str());
                    DeleteFileW(outputPath.c_str());
                    if (!parsed) {
                        outcome->error = worker.error.empty() ? error : worker.error;
                    } else {
                        outcome->result = core::ocr::BuildResult(std::move(worker.lines));
                    }
                }
            }
            if (!PostMessageW(notify, halocap::app::kOcrOutcomeMessage,
                              reinterpret_cast<WPARAM>(outcome), 0)) {
                delete outcome;
            }
        }).detach();
    };

    const auto pruneOcrWindows = [&ocrWindows]() {
        ocrWindows.erase(
            std::remove_if(ocrWindows.begin(), ocrWindows.end(),
                           [](const std::unique_ptr<halocap::app::OcrResultWindow>& window) {
                               return !window->Valid();
                           }),
            ocrWindows.end());
    };

    const auto openOcrResult = [&](const std::uint8_t* bgra, int width, int height, int x,
                                   int y, bool fromPin) {
        pruneOcrWindows();
        auto window = std::make_unique<halocap::app::OcrResultWindow>();
        halocap::app::OcrResultOptions resultOptions;
        resultOptions.engine = liveSettings.ocr.engine;
        resultOptions.autoCopy = liveSettings.ocr.autoCopy;
        resultOptions.fromPin = fromPin;
        resultOptions.anchor = core::IntRect{ x, y, x + width, y + height };
        if (!window->Create(nullptr, bgra, width, height, resultOptions, startOcr)) {
            log.Error("ocr: result window failed");
            return;
        }
        ocrWindows.push_back(std::move(window));
    };

    const auto wirePin = [&](halocap::app::PinWindow& pin) {
        pin.SetOcrHandler([&openOcrResult](const std::uint8_t* image, int imageWidth,
                                           int imageHeight, int screenX, int screenY) {
            openOcrResult(image, imageWidth, imageHeight, screenX, screenY, true);
        });
        // 回调在某个贴图的窗口过程里跑：这里只关窗口，不在回调里删 pins（对象还在栈上）。
        pin.SetCloseAllHandler([&pins] {
            for (auto& item : pins) {
                item->Close();
            }
        });
        pin.SetOutlineChangedHandler([&](bool enabled) {
            liveSettings.pinGlowEnabled = enabled;
            if (!core::SaveSettingsTo(store, liveSettings)) {
                log.Error(std::string("pin: save outline failed (") + store.LastErrorKind() + ")");
            }
            for (auto& item : pins) {
                item->SetOutline(enabled);
            }
        });
    };

    const auto openScreenshot = [&]() {
        if (screenshotOverlay != nullptr && screenshotOverlay->Active()) {
            return;
        }
        // 上一次的结果窗口保持常显（不关也不隐藏），用户可以对照着继续截下一张。
        pruneOcrWindows();
        screenshotOverlay = std::make_unique<halocap::app::ScreenshotOverlay>();
        halocap::app::ScreenshotOverlayOptions overlayOptions;
        overlayOptions.jpeg = settings.screenshotFormat == core::ScreenshotFormat::Jpeg;
        overlayOptions.pinHotkey = liveSettings.pinHotkey;
        overlayOptions.onPin = [&](const std::uint8_t* bgra, int width, int height, int x, int y) {
            auto pin = std::make_unique<halocap::app::PinWindow>();
            if (pin->Create(bgra, width, height, x, y, pinOptions())) {
                wirePin(*pin);
                pins.push_back(std::move(pin));
            }
        };
        // 截图工具栏的「识别文字」和 OCR 模式共用同一条入口。
        overlayOptions.onOcr = [&openOcrResult](const std::uint8_t* image, int imageWidth,
                                               int imageHeight, int x, int y) {
            openOcrResult(image, imageWidth, imageHeight, x, y, false);
        };

        if (!screenshotOverlay->Open(overlayOptions)) {
            log.Error(std::string("screenshot: overlay failed (") +
                      screenshotOverlay->LastErrorKind() + ")");
            screenshotOverlay.reset();
            notify("截图失败：无法获取屏幕");
            return;
        }
        log.Info("screenshot: overlay opened");
    };


    const auto openOcr = [&]() {
        if (screenshotOverlay != nullptr && screenshotOverlay->Active()) {
            return;
        }
        pruneOcrWindows();
        screenshotOverlay = std::make_unique<halocap::app::ScreenshotOverlay>();
        halocap::app::ScreenshotOverlayOptions overlayOptions;
        overlayOptions.mode = halocap::app::OverlayMode::Ocr;
        overlayOptions.autoStart = settings.ocr.autoStart;
        overlayOptions.onOcr = [&openOcrResult](const std::uint8_t* image, int imageWidth,
                                               int imageHeight, int x, int y) {
            openOcrResult(image, imageWidth, imageHeight, x, y, false);
        };
        overlayOptions.pinHotkey = liveSettings.pinHotkey;

        if (!screenshotOverlay->Open(overlayOptions)) {
            log.Error(std::string("ocr: overlay failed (") +
                      screenshotOverlay->LastErrorKind() + ")");
            screenshotOverlay.reset();
            notify("OCR 无法获取屏幕");
            return;
        }
        log.Info("ocr: overlay opened");
    };

    const auto openSettings = [&]() {
        if (settingsWindow != nullptr && settingsWindow->Valid()) {
            return;
        }
        settingsWindow = std::make_unique<halocap::app::SettingsWindow>();
        const bool created = settingsWindow->Create(
            nullptr, liveSettings, facts, ResolveOcrModelDirectory(),
            [&](const core::AppSettings& updated, std::wstring& error) {
                // 1.x 的顺序：先换热键，任何一步失败都退回原样、窗口不关、编辑保留。
                for (int slot = 0; slot < core::kHotkeySlotCount; ++slot) {
                    const int conflict = core::FindHotkeyConflict(updated, slot);
                    if (conflict >= 0) {
                        error = std::wstring(L"「") + kHotkeyLabels[slot] + L"」与「" +
                                kHotkeyLabels[conflict] + L"」使用了相同的快捷键，请更换其中一个。";
                        return false;
                    }
                }
                const core::AppSettings previous = liveSettings;
                const int previousRegistered = hotkeysRegistered;
                if (inputStarted) {
                    hotkeysRegistered = registerAllHotkeys(updated, &error);
                    if (!error.empty()) {
                        hotkeysRegistered = registerAllHotkeys(previous, nullptr);
                        if (hotkeysRegistered == 0) {
                            hotkeysRegistered = previousRegistered;
                        }
                        return false;
                    }
                }
                if (!core::SaveSettingsTo(store, updated)) {
                    log.Error(std::string("settings: save failed (") + store.LastErrorKind() + ")");
                    if (inputStarted) {
                        hotkeysRegistered = registerAllHotkeys(previous, nullptr);
                    }
                    error = L"无法保存设置，已恢复原快捷键。";
                    return false;
                }
                liveSettings = updated;
                std::string autoStartError;
                if (!halocap::platform::win32::SetAutoStart(updated.autoStart, selfPath,
                                                            &autoStartError)) {
                    log.Error("autostart: apply failed (" + autoStartError + ")");
                }
                trayState.autoStart = halocap::platform::win32::IsAutoStartEnabled(selfPath);
                tray.Apply(trayState);
                log.Info("settings: saved");
                notify("设置已保存（录制参数下次启动生效）");
                return true;
            });
        if (!created) {
            settingsWindow.reset();
            log.Error("settings: window failed");
        }
    };

    // 剪贴板贴图：读取 → core 决策 → 图片/卡片 → 分层贴图窗口。
    // planner 要跨多次粘贴保留：同一批文件图像贴出一次后，再贴应回退成文件路径文本。
    core::clipboard::PinPlanner pinPlanner;
    const auto pinClipboard = [&]() {
        halocap::platform::win32::ClipboardReadResult data;
        if (!halocap::platform::win32::ReadClipboard(&data)) {
            log.Error("pin: cannot read clipboard");
            notify("无法读取剪贴板");
            return;
        }

        core::clipboard::SnapshotInput input;
        input.sequence = data.sequence;
        input.filePaths = data.filePaths;
        input.hasImage = data.hasImage;
        input.hasHtml = data.hasHtml;
        input.html = data.html;
        input.hasText = data.hasText;
        input.text = data.text;
        const core::clipboard::Snapshot snapshot = core::clipboard::NormalizeSnapshot(input);
        if (!snapshot.HasContent()) {
            notify("剪贴板里没有可贴图的内容");
            return;
        }

        core::clipboard::PinPlanner planner;
        struct Item {
            halocap::platform::win32::ClipboardImage image;
        };
        std::vector<Item> items;

        core::clipboard::PinOptions filePinOptions;
        filePinOptions.convertFilePathsToImages = settings.convertFilePathsToImages;
        if (!snapshot.filePaths.empty()) {
            std::unordered_map<std::string, halocap::platform::win32::ClipboardImage> loaded;
            const auto loader = [&](const std::string& path) {
                halocap::platform::win32::ClipboardImage image;
                if (!halocap::platform::win32::LoadImageFile(path, 256LL * 1024 * 1024, &image)) {
                    return false;
                }
                loaded[path] = std::move(image);
                return true;
            };
            const auto plans = pinPlanner.PlanFiles(snapshot, filePinOptions, loader);
            for (const auto& plan : plans) {
                if (plan.kind == core::clipboard::PinKind::FileImage) {
                    items.push_back(Item{ std::move(loaded[plan.text]) });
                    pinPlanner.MarkPasted(plan);
                } else {
                    halocap::platform::win32::ClipboardImage card;
                    if (halocap::platform::win32::RenderTextCard(plan.text, 1.0, &card)) {
                        items.push_back(Item{ std::move(card) });
                    }
                }
            }
        } else {
            const auto plans = pinPlanner.PlanContent(snapshot);
            for (const auto& plan : plans) {
                if (plan.kind == core::clipboard::PinKind::Image) {
                    items.push_back(Item{ data.image });
                } else if (plan.kind == core::clipboard::PinKind::Color) {
                    core::clipboard::Color color;
                    halocap::platform::win32::ClipboardImage card;
                    if (core::clipboard::ParseColor(plan.text, &color) &&
                        halocap::platform::win32::RenderColorCard(color, 1.0, &card)) {
                        items.push_back(Item{ std::move(card) });
                    }
                } else {
                    halocap::platform::win32::ClipboardImage card;
                    if (halocap::platform::win32::RenderTextCard(plan.text, 1.0, &card)) {
                        items.push_back(Item{ std::move(card) });
                    }
                }
            }
        }

        int created = 0;
        int offset = 0;
        for (Item& item : items) {
            if (item.image.width <= 0 || item.image.height <= 0) {
                continue;
            }
            auto pin = std::make_unique<halocap::app::PinWindow>();
            if (pin->Create(item.image.bgra.data(), item.image.width, item.image.height,
                            180 + offset, 160 + offset, pinOptions())) {
                wirePin(*pin);
                pins.push_back(std::move(pin));
                ++created;
                offset += 28;
            }
        }

        log.Info("pin: " + std::to_string(created) + " item(s) from clipboard");
        if (created == 0) {
            notify("贴图失败");
        }
    };

    auto onAction = [&](core::TrayCommand command) {
        ++actions;
        if (command == core::TrayCommand::ToggleAutoStart) {
            liveSettings.autoStart = !liveSettings.autoStart;
            std::string autoStartError;
            halocap::platform::win32::SetAutoStart(liveSettings.autoStart, selfPath,
                                                   &autoStartError);
            trayState.autoStart = halocap::platform::win32::IsAutoStartEnabled(selfPath);
            tray.Apply(trayState);
            log.Info(liveSettings.autoStart ? "autostart: enabled" : "autostart: disabled");
            return;
        }
        if (command == core::TrayCommand::Exit) {
            log.Info("action: Exit");
            running = false;
            return;
        }
        if (command == core::TrayCommand::Record) {
            toggleRecording();
            return;
        }
        if (command == core::TrayCommand::Screenshot) {
            openScreenshot();
            return;
        }
        if (command == core::TrayCommand::Ocr) {
            openOcr();
            return;
        }
        if (command == core::TrayCommand::Settings) {
            openSettings();
            return;
        }
        if (command == core::TrayCommand::Pin) {
            pinClipboard();
            return;
        }
        if (command == core::TrayCommand::PauseRecording) {
            togglePause();
            return;
        }
        log.Info(std::string("action: ") + core::TrayCommandName(command) +
                 " (behaviour lands in S4)");
    };

    const auto openRadial = [&]() {
        // 光圈已经开着时再按快捷键：先关掉旧的，再按当前光标位置重新呼出。
        if (radialMenu != nullptr) {
            radialMenu->Close();
            radialMenu.reset();
        }
        radialMenu = std::make_unique<halocap::app::RadialMenu>();
        const bool opened = radialMenu->Open([&](core::AppAction action) {
            switch (action) {
            case core::AppAction::Screenshot:
                openScreenshot();
                break;
            case core::AppAction::Ocr:
                openOcr();
                break;
            case core::AppAction::Record:
                toggleRecording();
                break;
            case core::AppAction::Pin:
                pinClipboard();
                break;
            default:
                break;
            }
        }, settings.menuSize);
        if (!opened) {
            radialMenu.reset();
            log.Error("radial: open failed");
        }
    };
    bool screenshotSelftestOk = true;
    if (!options.screenshotSelftestPath.empty()) {
        const std::int64_t openStartUs = QpcNowUs();
        openScreenshot();
        const double openMs = static_cast<double>(QpcNowUs() - openStartUs) / 1000.0;
        if (screenshotOverlay == nullptr || !screenshotOverlay->Active()) {
            screenshotSelftestOk = false;
            log.Error("screenshot selftest: overlay did not open");
        } else {
            // 走真实的鼠标消息路径。坐标都用「客户区」，选区结果换算回客户区再断言，
            // 这样多屏时窗口原点不是 (0,0) 也不影响。
            HWND overlayWindow = static_cast<HWND>(screenshotOverlay->WindowHandle());
            RECT overlayRect{};
            GetWindowRect(overlayWindow, &overlayRect);
            const auto toClient = [&](const core::IntRect& rect) {
                return core::IntRect{ rect.left - overlayRect.left, rect.top - overlayRect.top,
                                      rect.right - overlayRect.left,
                                      rect.bottom - overlayRect.top };
            };
            const auto sendMouse = [&](UINT message, int x, int y) {
                SendMessageW(overlayWindow, message, 0, MAKELPARAM(x, y));
            };
            // kMsgAnnotate = WM_APP + 11：空格那条路（切标注态）的自检入口。
            constexpr UINT kMsgAnnotateForTest = WM_APP + 11;
            constexpr UINT kMsgNudgeForTest = WM_APP + 12;
            const auto pumpMessages = [] {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            };
            // 客户区原点对应虚拟桌面左上角，主屏左上角不一定在那里（本机主屏在下面）。
            const int baseX = -static_cast<int>(overlayRect.left);
            const int baseY = -static_cast<int>(overlayRect.top);
            const int x0 = baseX + 200;
            const int y0 = baseY + 200;
            const int x1 = baseX + 2000;
            const int y1 = baseY + 600;

            // 0) 空闲悬停：十字线 + 光标下窗口描边 + 放大镜，抓一张证明图。
            //    覆盖层刚打开、还没收到任何鼠标消息时就必须有候选（1.x 的自动识别窗口）。
            const core::IntRect hoverAtOpen = screenshotOverlay->HoverRectForTest();
            const bool hoverDetectOk = hoverAtOpen.right - hoverAtOpen.left >= 32 &&
                                       hoverAtOpen.bottom - hoverAtOpen.top >= 32;
            //    自检没有消息循环，脏帧得自己 Pump 一次才会 Present。
            //    先等过节流窗口（24 ms），否则这次移动可能被折叠成「没动」。
            Sleep(40);
            sendMouse(WM_MOUSEMOVE, baseX + 500, baseY + 500);
            screenshotOverlay->Pump();
            Sleep(150);
            const core::IntRect hoverMoved = screenshotOverlay->HoverRectForTest();
            // baseX/baseY 已经把窗口原点抵消了：客户区 (baseX+500, baseY+500) 就是屏幕 (500,500)。
            constexpr int hoverCursorX = 500;
            constexpr int hoverCursorY = 500;
            const bool hoverUnderCursor =
                hoverMoved.left <= hoverCursorX && hoverCursorX < hoverMoved.right &&
                hoverMoved.top <= hoverCursorY && hoverCursorY < hoverMoved.bottom;
            const bool hoverProof =
                screenshotOverlay->GrabDesktopToPng(options.screenshotSelftestPath + L".hover.png");

            // 1) 拖选 600×400。
            sendMouse(WM_MOUSEMOVE, x0, y0);
            sendMouse(WM_LBUTTONDOWN, x0, y0);
            sendMouse(WM_MOUSEMOVE, x1, y1);
            sendMouse(WM_LBUTTONUP, x1, y1);
            const core::IntRect dragged = toClient(screenshotOverlay->SelectionRectPx());
            const bool dragOk = dragged.left == x0 && dragged.top == y0 &&
                                dragged.right == x1 && dragged.bottom == y1;

            // 2) 刚拖完不是标注态：在选区里按下拖动 = 整体平移，尺寸不变。
            sendMouse(WM_LBUTTONDOWN, x0 + 300, y0 + 200);
            sendMouse(WM_MOUSEMOVE, x0 + 350, y0 + 230);
            sendMouse(WM_LBUTTONUP, x0 + 350, y0 + 230);
            const core::IntRect moved = toClient(screenshotOverlay->SelectionRectPx());
            const bool moveOk = moved.left == x0 + 50 && moved.top == y0 + 30 &&
                                moved.right - moved.left == 1800 && moved.bottom - moved.top == 400;

            // 3) 右下把手缩放 +40/+20。
            sendMouse(WM_LBUTTONDOWN, moved.right, moved.bottom);
            sendMouse(WM_MOUSEMOVE, moved.right + 40, moved.bottom + 20);
            sendMouse(WM_LBUTTONUP, moved.right + 40, moved.bottom + 20);
            const core::IntRect resized = toClient(screenshotOverlay->SelectionRectPx());
            const bool resizeOk = resized.left == x0 + 50 && resized.top == y0 + 30 &&
                                  resized.right - resized.left == 1840 &&
                                  resized.bottom - resized.top == 420;

            // 4) 方向键微调 1 px（Shift 才是 10 px）。
            PostMessageW(overlayWindow, kMsgNudgeForTest, VK_RIGHT, 1);
            pumpMessages();
            const core::IntRect nudged = toClient(screenshotOverlay->SelectionRectPx());
            const bool nudgeOk = nudged.left == x0 + 51 && nudged.right - nudged.left == 1840;

            // 5) 工具栏按 1.x 对齐：拖完才出现、高 38、右边缘贴选区右边缘，
            //    按钮顺序 = 选择 | 形状 箭头 | 笔 荧光 橡皮 文字 马赛克 序号 |
            //    撤 重 清空 | 存 贴 识别 | 取消 完成（共 17 个 + 5 条分隔线）。
            screenshotOverlay->Pump();
            const core::IntRect toolbar = screenshotOverlay->ToolbarBoundsForTest();
            const int uiScalePct = screenshotOverlay->UiScalePercentForTest();
            const bool toolbarOk = screenshotOverlay->ToolbarVisibleForTest() &&
                                   toolbar.bottom - toolbar.top == 38 * uiScalePct / 100 &&
                                   toolbar.right == nudged.right &&
                                   screenshotOverlay->ToolbarButtonCountForTest() == 22;
            AddInt(report, "APP_SCREENSHOT_UI_SCALE_PCT", uiScalePct);
            // 形状工具：选项栏应当出现（8 色 + 分隔 + 矩形/椭圆/空心/实心 + 粗细加减）。
            // 直接点工具栏里的「形状」按钮（第 3 个条目），验证工具栏真的能点。
            const core::IntRect shapeButton = screenshotOverlay->ToolbarButtonRectForTest(2);
            bool clickShapeOk = false;
            if (!shapeButton.IsEmpty()) {
                // 按钮矩形是客户区坐标，WM_* 的 lParam 也是客户区坐标，直接送。
                const int shapeX = shapeButton.left + (shapeButton.right - shapeButton.left) / 2;
                const int shapeY = shapeButton.top + (shapeButton.bottom - shapeButton.top) / 2;
                sendMouse(WM_LBUTTONDOWN, shapeX, shapeY);
                sendMouse(WM_LBUTTONUP, shapeX, shapeY);
                pumpMessages();
                clickShapeOk = screenshotOverlay->AnnotatingForTest();
            }
            screenshotOverlay->Pump();
            const bool optionsOk = screenshotOverlay->AnnotatingForTest() &&
                                   screenshotOverlay->OptionButtonCountForTest() == 16;
            // 回到移动态，下一步的右键才是「清选区」而不是「退出标注」。
            PostMessageW(overlayWindow, WM_APP + 9, 0, 0);
            pumpMessages();

            // 5) 右键：有选区时先清掉选区，不关窗口。
            SendMessageW(overlayWindow, WM_RBUTTONDOWN, MK_RBUTTON,
                         MAKELPARAM(x0 + 300, y0 + 200));
            const bool rightClickOk = screenshotOverlay->SelectionRectPx().IsEmpty() &&
                                      screenshotOverlay->Active();

            // 6) 单击（短于 8 px 的拖）= 选光标下的窗口；没窗口就选整块屏。
            sendMouse(WM_LBUTTONDOWN, baseX + 40, baseY + 40);
            sendMouse(WM_MOUSEMOVE, baseX + 42, baseY + 42);
            sendMouse(WM_LBUTTONUP, baseX + 42, baseY + 42);
            const core::IntRect clicked = screenshotOverlay->SelectionRectPx();
            const bool clickSelectOk = !clicked.IsEmpty() &&
                                       clicked.right - clicked.left > 16 &&
                                       clicked.bottom - clicked.top > 16;

            // 7) 先右键清掉上一步那个窗口选区，再重新拖一块、切进标注态画（标注态下鼠标才落笔）。
            SendMessageW(overlayWindow, WM_RBUTTONDOWN, MK_RBUTTON,
                         MAKELPARAM(baseX + 300, baseY + 300));
            sendMouse(WM_LBUTTONDOWN, x0, y0);
            sendMouse(WM_MOUSEMOVE, x1, y1);
            sendMouse(WM_LBUTTONUP, x1, y1);
            // 工具键（P）走 1.x 的字母快捷键那条消息路径。
            PostMessageW(overlayWindow, WM_APP + 6, 0, 0);
            pumpMessages();
            screenshotOverlay->Pump();
            // 选项栏必须完整包在背景里（跑出去的按钮既看不见、也点不到）；滚轮要能调粗细。
            const bool optionsFitOk = screenshotOverlay->OptionBarFitsForTest();
            const float thicknessBefore = screenshotOverlay->ThicknessForTest();
            SendMessageW(overlayWindow, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                         MAKELPARAM(x0 + 120, y0 + 120));
            pumpMessages();
            const bool wheelOk = screenshotOverlay->ThicknessForTest() > thicknessBefore;
            sendMouse(WM_LBUTTONDOWN, x0 + 60, y0 + 60);
            for (int x = x0 + 70; x <= x0 + 320; x += 10) {
                sendMouse(WM_MOUSEMOVE, x, y0 + 70);
            }
            screenshotOverlay->Pump();
            // DComp 是异步上屏：Pump 之后要等一下，抓到的才是这一帧。
            Sleep(150);
            const bool drawingProof =
                screenshotOverlay->GrabDesktopToPng(options.screenshotSelftestPath + L".drawing.png");
            sendMouse(WM_LBUTTONUP, x0 + 320, y0 + 70);
            const bool annotated = !screenshotOverlay->Document().Items().empty();
            (void)drawingProof;

            core::AnnotationStyle style;
            style.color = core::annotation_defaults::PaletteColor(0);
            style.thickness = 4.0f;
            core::AnnotationDocument& document = screenshotOverlay->Document();

            document.Begin(core::AnnotationTool::Pen, style);
            for (int i = 0; i <= 40; ++i) {
                document.AddPoint(core::Point{ 40.0f + i * 4.0f, 60.0f });
            }
            document.Commit();

            style.color = core::annotation_defaults::PaletteColor(4);
            document.Begin(core::AnnotationTool::Rectangle, style);
            document.SetEnd(core::Point{ 40.0f, 120.0f });
            document.SetEnd(core::Point{ 240.0f, 220.0f });
            document.Commit();

            style.color = core::annotation_defaults::PaletteColor(0);
            document.Begin(core::AnnotationTool::Arrow, style);
            document.SetEnd(core::Point{ 40.0f, 300.0f });
            document.SetEnd(core::Point{ 240.0f, 300.0f });
            document.Commit();

            style.color = core::annotation_defaults::PaletteColor(3);
            document.Begin(core::AnnotationTool::Step, style);
            document.AddPoint(core::Point{ 500.0f, 60.0f });
            document.Commit();

            screenshotOverlay->AddTextAnnotation(core::Point{ 40.0f, 360.0f }, "HaloCap S3");

            // 马赛克取样必须包含下方标注：红色笔迹在选区局部 y=60、x=40..200，
            // 在 (120,60) 抹一笔，合成缓存里那一点必须是红色，而不是底图。
            PostMessageW(overlayWindow, WM_APP + 6, 6, 0);  // kTools[6] = 马赛克
            pumpMessages();
            sendMouse(WM_LBUTTONDOWN, x0 + 120, y0 + 60);
            sendMouse(WM_MOUSEMOVE, x0 + 132, y0 + 60);
            sendMouse(WM_LBUTTONUP, x0 + 132, y0 + 60);
            pumpMessages();
            std::uint8_t mosaicPixel[4] = {};
            const bool mosaicComposeOk =
                screenshotOverlay->MosaicComposeValidForTest() &&
                screenshotOverlay->MosaicBasePixelForTest(120.0f, 60.0f, mosaicPixel);
            const bool mosaicOverAnnotationOk =
                mosaicComposeOk && mosaicPixel[2] > 190 && mosaicPixel[1] < 150 &&
                mosaicPixel[0] < 150 && !document.Items().empty() &&
                document.Items().back().tool == core::AnnotationTool::Mosaic &&
                document.Items().back().mosaicTile != 0;

            // 已经有标注时，点选区外不能重开选区（那会把标注全丢掉）。
            const core::IntRect beforeOutside = screenshotOverlay->SelectionRectPx();
            const std::size_t itemsBeforeOutside = document.Items().size();
            sendMouse(WM_LBUTTONDOWN, baseX + 40, baseY + 40);
            sendMouse(WM_LBUTTONUP, baseX + 40, baseY + 40);
            pumpMessages();
            const core::IntRect afterOutside = screenshotOverlay->SelectionRectPx();
            const bool outsideGuardOk =
                afterOutside.left == beforeOutside.left &&
                afterOutside.top == beforeOutside.top &&
                afterOutside.right == beforeOutside.right &&
                afterOutside.bottom == beforeOutside.bottom &&
                document.Items().size() == itemsBeforeOutside;

            screenshotOverlay->Pump();
            Sleep(250);
            const double screenshotMb = PrivateMb();

            const bool proof = screenshotOverlay->GrabDesktopToPng(options.screenshotSelftestPath);
            std::string savedPath;
            const bool saved = screenshotOverlay->SaveSelection(&savedPath);
            // 导出的 PNG 必须带标注：读回来检查红色笔迹经过的那一点（局部 60,60）是纯红。
            // 马赛克块在 x 84..156，60 在它左边，读到的是笔迹本身而不是块平均色。
            halocap::platform::win32::ClipboardImage exported;
            bool exportAnnotated = false;
            if (saved && halocap::platform::win32::LoadImageFile(
                             savedPath, 64LL * 1024 * 1024, &exported) &&
                exported.width > 60 && exported.height > 60 &&
                exported.bgra.size() >
                    (static_cast<std::size_t>(60) * static_cast<std::size_t>(exported.width) + 60) *
                        4 + 3) {
                const std::size_t offset =
                    (static_cast<std::size_t>(60) * static_cast<std::size_t>(exported.width) + 60) *
                    4;
                const std::uint8_t* pixel = exported.bgra.data() + offset;
                exportAnnotated = pixel[2] > 190 && pixel[1] < 150 && pixel[0] < 150;
            }
            const bool pinned = screenshotOverlay->PinSelection();

            // 截图模式的「识别文字」按钮：点了应当把选区交给 OCR 结果窗口并关闭覆盖层。
            bool ocrButtonOk = false;
            const core::IntRect ocrButtonRect = screenshotOverlay->ToolbarButtonRectForTest(18);
            if (!ocrButtonRect.IsEmpty()) {
                const int ocrX = ocrButtonRect.left + (ocrButtonRect.right - ocrButtonRect.left) / 2;
                const int ocrY = ocrButtonRect.top + (ocrButtonRect.bottom - ocrButtonRect.top) / 2;
                sendMouse(WM_LBUTTONDOWN, ocrX, ocrY);
                sendMouse(WM_LBUTTONUP, ocrX, ocrY);
                ocrButtonOk = !ocrWindows.empty() && ocrWindows.back()->Valid();
            }
            for (auto& window : ocrWindows) {
                window->Close();
            }
            ocrWindows.clear();
            screenshotOverlay->Close();
            // 贴图证明图：覆盖层关掉之后再抓，窗口与光晕才是可见的。
            Sleep(150);
            const bool pinProof =
                screenshotOverlay->GrabDesktopToPng(options.screenshotSelftestPath + L".pin.png");
            // S3 的退出条件是「截图后回落 ≤ 120 MB」——量的是关掉覆盖层之后，不是覆盖层开着的时候。
            TrimProcessMemory();
            Sleep(150);
            const double afterCloseMb = PrivateMb();

            // 预算按像素量给：整块虚拟桌面（本机 6400×2160 ≈ 13.8 MP）比单屏 2560×1440 大 3.75 倍，
            // 抓图、D2D 纹理和窗口都按面积走，固定阈值会误判。
            const double megapixels =
                static_cast<double>(overlayRect.right - overlayRect.left) *
                static_cast<double>(overlayRect.bottom - overlayRect.top) / 1'000'000.0;
            const double openBudgetMs = 90.0 + 40.0 * megapixels;
            const double overlayBudgetMb = 40.0 + 14.0 * megapixels;
            const bool latencyOk = openMs <= openBudgetMs;
            const bool overlayMemoryOk = screenshotMb <= overlayBudgetMb;
            const bool afterCloseOk = afterCloseMb > 0.0 && afterCloseMb <= 120.0;

            AddFlag(report, "APP_SCREENSHOT_OVERLAY", true);
            AddFlag(report, "APP_SCREENSHOT_PROOF", proof);
            AddFlag(report, "APP_SCREENSHOT_PIN_PROOF", pinProof);
            AddFlag(report, "APP_SCREENSHOT_SAVED", saved);
            AddFlag(report, "APP_SCREENSHOT_PINNED", pinned);
            AddInt(report, "APP_SCREENSHOT_PINS", static_cast<long long>(pins.size()));
            AddText(report, "APP_SCREENSHOT_PATH", savedPath.c_str());
            char openBuffer[32] = {};
            std::snprintf(openBuffer, sizeof(openBuffer), "%.1f", openMs);
            AddText(report, "APP_SCREENSHOT_OPEN_MS", openBuffer);
            AddMb(report, "APP_SCREENSHOT_PRIVATE_MB", screenshotMb);
            AddInt(report, "APP_SCREENSHOT_ITEMS",
                   static_cast<long long>(document.Items().size()));
            AddFlag(report, "APP_SCREENSHOT_SELECT_DRAG", dragOk);
            AddFlag(report, "APP_SCREENSHOT_SELECT_MOVE", moveOk);
            AddFlag(report, "APP_SCREENSHOT_SELECT_RESIZE", resizeOk);
            AddFlag(report, "APP_SCREENSHOT_SELECT_NUDGE", nudgeOk);
            AddFlag(report, "APP_SCREENSHOT_SELECT_RIGHT_CLICK", rightClickOk);
            AddFlag(report, "APP_SCREENSHOT_SELECT_CLICK", clickSelectOk);
            AddFlag(report, "APP_SCREENSHOT_TOOLBAR", toolbarOk);
            AddFlag(report, "APP_SCREENSHOT_TOOLBAR_CLICK", clickShapeOk);
            AddFlag(report, "APP_SCREENSHOT_OPTIONBAR", optionsOk);
            AddFlag(report, "APP_SCREENSHOT_OPTIONBAR_FITS", optionsFitOk);
            AddFlag(report, "APP_SCREENSHOT_WHEEL", wheelOk);
            AddFlag(report, "APP_SCREENSHOT_ANNOTATE", annotated);
            AddFlag(report, "APP_SCREENSHOT_HOVER_DETECT", hoverDetectOk);
            AddFlag(report, "APP_SCREENSHOT_HOVER_UNDER_CURSOR", hoverUnderCursor);
            char hoverRectText[64] = {};
            std::snprintf(hoverRectText, sizeof(hoverRectText), "%d,%d %dx%d", hoverMoved.left,
                          hoverMoved.top, hoverMoved.right - hoverMoved.left,
                          hoverMoved.bottom - hoverMoved.top);
            AddText(report, "APP_SCREENSHOT_HOVER_RECT", hoverRectText);
            AddFlag(report, "APP_SCREENSHOT_MOSAIC_COMPOSE", mosaicComposeOk);
            AddFlag(report, "APP_SCREENSHOT_MOSAIC_OVER_ANNOTATION", mosaicOverAnnotationOk);
            AddFlag(report, "APP_SCREENSHOT_OUTSIDE_GUARD", outsideGuardOk);
            AddFlag(report, "APP_SCREENSHOT_OCR_BUTTON", ocrButtonOk);
            AddFlag(report, "APP_SCREENSHOT_EXPORT_ANNOTATED", exportAnnotated);
            AddFlag(report, "APP_SCREENSHOT_HOVER_PROOF", hoverProof);
            AddFlag(report, "APP_SCREENSHOT_LATENCY_OK", latencyOk);
            AddFlag(report, "APP_SCREENSHOT_OVERLAY_MEMORY_OK", overlayMemoryOk);
            AddFlag(report, "APP_SCREENSHOT_AFTER_CLOSE_OK", afterCloseOk);
            AddMb(report, "APP_SCREENSHOT_AFTER_CLOSE_MB", afterCloseMb);
            char budgetBuffer[48] = {};
            std::snprintf(budgetBuffer, sizeof(budgetBuffer), "%.1f/%.1f", openMs, openBudgetMs);
            AddText(report, "APP_SCREENSHOT_OPEN_BUDGET_MS", budgetBuffer);
            std::snprintf(budgetBuffer, sizeof(budgetBuffer), "%.1f/%.1f", screenshotMb,
                          overlayBudgetMb);
            AddText(report, "APP_SCREENSHOT_MEMORY_BUDGET_MB", budgetBuffer);

            screenshotSelftestOk = proof && pinProof && saved && pinned && !pins.empty() &&
                                   latencyOk && overlayMemoryOk && afterCloseOk && dragOk &&
                                   moveOk && resizeOk && nudgeOk && rightClickOk && clickSelectOk &&
                                   annotated && toolbarOk && optionsOk && clickShapeOk &&
                                   optionsFitOk && wheelOk && mosaicComposeOk &&
                                   mosaicOverAnnotationOk && exportAnnotated && hoverDetectOk &&
                                   hoverUnderCursor && outsideGuardOk && ocrButtonOk;
            log.Info(screenshotSelftestOk ? "screenshot selftest: ok"
                                          : "screenshot selftest: failed");
        }
        running = false;
    }

    // OCR 自检：OCR 覆盖层 → 选区 → 识别 → 结果窗口 → 抓一张带结果窗口的证明图。
    bool ocrSelftestOk = true;
    int ocrSelftestLines = 0;
    if (!options.ocrSelftestPath.empty()) {
        ocrSelftestOk = false;
        const std::int64_t openStart = QpcNowUs();
        openOcr();
        const double ocrOpenMs = static_cast<double>(QpcNowUs() - openStart) / 1000.0;

        if (screenshotOverlay != nullptr && screenshotOverlay->Active()) {
            // 走真实鼠标消息拖一个选区：1.x 的 OCR 模式松手即识别，不应该再点「识别」。
            const HWND overlayWindow = static_cast<HWND>(screenshotOverlay->WindowHandle());
            RECT overlayRect{};
            GetWindowRect(overlayWindow, &overlayRect);
            const int dragX0 = 100 - overlayRect.left;
            const int dragY0 = 100 - overlayRect.top;
            const int dragX1 = 1000 - overlayRect.left;
            const int dragY1 = 620 - overlayRect.top;
            const double beforeOcrMb = PrivateMb();
            SendMessageW(overlayWindow, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(dragX0, dragY0));
            SendMessageW(overlayWindow, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(dragX1, dragY1));
            SendMessageW(overlayWindow, WM_LBUTTONUP, 0, MAKELPARAM(dragX1, dragY1));

            // 识别在后台线程跑 worker：结果窗口先以 loading 打开，等它进结果态再断言。
            halocap::app::OcrResultWindow* ocr =
                ocrWindows.empty() ? nullptr : ocrWindows.back().get();
            const bool windowOpen = ocr != nullptr && ocr->Valid();
            const std::uint64_t waitStart = GetTickCount64();
            while (ocr != nullptr && ocr->Valid() && !ocr->ReadyForTest() &&
                   GetTickCount64() - waitStart < 8000) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(20);
            }
            const bool recognized = ocr != nullptr && ocr->ReadyForTest();
            ocrSelftestLines = ocr != nullptr ? ocr->LineCountForTest() : 0;
            const double ocrRecognizeMs = ocr != nullptr ? ocr->ElapsedMsForTest() : 0.0;
            Sleep(400);
            RECT ocrWindowRect{};
            bool ocrWindowVisible = false;
            bool ocrWindowNormalLevel = false;
            if (ocr != nullptr && ocr->Valid()) {
                GetWindowRect(ocr->Handle(), &ocrWindowRect);
                ocrWindowVisible = IsWindowVisible(ocr->Handle()) != FALSE;
                // 结果窗是普通层级：不能带 WS_EX_TOPMOST（否则会一直压在对照内容上面）。
                const LONG_PTR exStyle = GetWindowLongPtrW(ocr->Handle(), GWL_EXSTYLE);
                ocrWindowNormalLevel = (exStyle & WS_EX_TOPMOST) == 0;
            }
            AddFlag(report, "APP_OCR_WINDOW_VISIBLE", ocrWindowVisible);
            AddInt(report, "APP_OCR_TEXT_LINES_CTRL",
                   ocr != nullptr ? ocr->TextLineCountForTest() : -1);
            AddFlag(report, "APP_OCR_TEXT_SCROLLBAR",
                    ocr != nullptr && ocr->TextScrollVisibleForTest());
            AddFlag(report, "APP_OCR_WINDOW_NORMAL_LEVEL", ocrWindowNormalLevel);
            AddText(report, "APP_OCR_WINDOW_RECT",
                    (std::to_string(ocrWindowRect.left) + "," + std::to_string(ocrWindowRect.top) +
                     " " + std::to_string(ocrWindowRect.right - ocrWindowRect.left) + "x" +
                     std::to_string(ocrWindowRect.bottom - ocrWindowRect.top)).c_str());
            // 证明图：普通层级窗口会被前台浏览器盖住，抓图前临时置顶，抓完恢复。
            if (ocr != nullptr && ocr->Valid()) {
                const LONG_PTR style = GetWindowLongPtrW(ocr->Handle(), GWL_EXSTYLE);
                SetWindowLongPtrW(ocr->Handle(), GWL_EXSTYLE, style | WS_EX_TOPMOST);
                SetWindowPos(ocr->Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                UpdateWindow(ocr->Handle());
                Sleep(200);
            }
            const bool proof = screenshotOverlay->GrabDesktopToPng(options.ocrSelftestPath);
            if (ocr != nullptr && ocr->Valid()) {
                const LONG_PTR style = GetWindowLongPtrW(ocr->Handle(), GWL_EXSTYLE);
                SetWindowLongPtrW(ocr->Handle(), GWL_EXSTYLE, style & ~WS_EX_TOPMOST);
                SetWindowPos(ocr->Handle(), HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            // 结果窗口自身再印一张：桌面抓图看不到它时，用这张判断是「没画」还是「被遮」。
            if (ocr != nullptr && ocr->Valid()) {
                RECT wr{};
                GetWindowRect(ocr->Handle(), &wr);
                const int ww = wr.right - wr.left;
                const int wh = wr.bottom - wr.top;
                HDC windowDc = GetWindowDC(ocr->Handle());
                HDC memory = CreateCompatibleDC(windowDc);
                BITMAPINFO wbmi{};
                wbmi.bmiHeader.biSize = sizeof(wbmi.bmiHeader);
                wbmi.bmiHeader.biWidth = ww;
                wbmi.bmiHeader.biHeight = -wh;
                wbmi.bmiHeader.biPlanes = 1;
                wbmi.bmiHeader.biBitCount = 32;
                wbmi.bmiHeader.biCompression = BI_RGB;
                void* wbits = nullptr;
                HBITMAP wbitmap =
                    CreateDIBSection(windowDc, &wbmi, DIB_RGB_COLORS, &wbits, nullptr, 0);
                if (wbitmap != nullptr && wbits != nullptr) {
                    HGDIOBJ oldW = SelectObject(memory, wbitmap);
                    PrintWindow(ocr->Handle(), memory, 0);
                    halocap::platform::win32::SaveBgraPng(
                        options.ocrSelftestPath + L".window.png",
                        static_cast<const std::uint8_t*>(wbits), ww, wh, ww * 4);
                    SelectObject(memory, oldW);
                    DeleteObject(wbitmap);
                }
                DeleteDC(memory);
                ReleaseDC(ocr->Handle(), windowDc);
            }
            const double ocrMb = PrivateMb();
            AddFlag(report, "APP_OCR_OVERLAY", true);
            AddFlag(report, "APP_OCR_WINDOW", windowOpen);
            AddFlag(report, "APP_OCR_RECOGNIZED", recognized);
            AddFlag(report, "APP_OCR_PROOF", proof);
            AddInt(report, "APP_OCR_LINES", ocrSelftestLines);
            char openBuffer[32] = {};
            std::snprintf(openBuffer, sizeof(openBuffer), "%.1f", ocrOpenMs);
            AddText(report, "APP_OCR_OPEN_MS", openBuffer);
            std::snprintf(openBuffer, sizeof(openBuffer), "%.1f", ocrRecognizeMs);
            AddText(report, "APP_OCR_RECOGNIZE_MS", openBuffer);
            AddMb(report, "APP_OCR_BEFORE_MB", beforeOcrMb);
            AddMb(report, "APP_OCR_PRIVATE_MB", ocrMb);

            // 关掉结果窗口、释放 ORT 会话之后必须回到空闲预算内（S4 的退出条件）。
            for (auto& window : ocrWindows) {
                window->Close();
            }
            ocrWindows.clear();
            TrimProcessMemory();
            Sleep(200);
            const double idleMb = PrivateMb();
            AddMb(report, "APP_OCR_AFTER_RELEASE_MB", idleMb);

            // 覆盖层在 OCR 松手时就同步关了；这里只再压一轮内存确认设备回收。
            if (screenshotOverlay != nullptr) {
                screenshotOverlay->Close();
                screenshotOverlay.reset();
            }
            TrimProcessMemory();
            Sleep(400);
            // 多屏之后这一块是整块虚拟桌面的 D3D 纹理，驱动侧回收是异步的：再等一轮再压一次。
            TrimProcessMemory();
            Sleep(200);
            const double afterOverlayMb = PrivateMb();
            AddMb(report, "APP_OCR_AFTER_OVERLAY_CLOSE_MB", afterOverlayMb);
            AddFlag(report, "APP_OVERLAY_PREWARMED", overlayReady);
            AddFlag(report, "APP_OCR_WORKER", true);
            AddText(report, "APP_OCR_ENGINE", core::OcrEngineName(liveSettings.ocr.engine));
            AddFlag(report, "APP_OCR_AFTER_RELEASE_UNDER_80", idleMb > 0.0 && idleMb <= 80.0);
            // 跨屏之后覆盖层是一整块虚拟桌面的纹理，驱动侧回收按面积走；判据也按面积给预算。
            const double desktopMegapixels =
                static_cast<double>(GetSystemMetrics(SM_CXVIRTUALSCREEN)) *
                static_cast<double>(GetSystemMetrics(SM_CYVIRTUALSCREEN)) / 1'000'000.0;
            const double overlayBudgetMb = 40.0 + 4.0 * desktopMegapixels;
            const bool overlayMemoryOk =
                afterOverlayMb > 0.0 && afterOverlayMb <= overlayBudgetMb;
            AddFlag(report, "APP_OCR_AFTER_OVERLAY_UNDER_BUDGET", overlayMemoryOk);
            char overlayBudget[48] = {};
            std::snprintf(overlayBudget, sizeof(overlayBudget), "%.1f/%.1f", afterOverlayMb,
                          overlayBudgetMb);
            AddText(report, "APP_OCR_OVERLAY_BUDGET_MB", overlayBudget);
            AddFlag(report, "APP_OCR_AFTER_OVERLAY_CLOSE_UNDER_80",
                    afterOverlayMb > 0.0 && afterOverlayMb <= 80.0);

            // 结果窗口常显：开一个真实的新 OCR 会话、再取消，旧窗口必须始终可见且对象有效
            // （不会因为下一次截图被关掉或隐藏）。放在内存闸门之后再建第二台覆盖层，
            // 避免驱动延迟回收污染上面的空闲内存数字。
            bool visibleBeforeCapture = false;
            bool visibleDuringCapture = false;
            bool persistsOk = false;
            {
                std::vector<std::uint8_t> probeImage(64 * 32 * 4, 255);
                openOcrResult(probeImage.data(), 64, 32, 100, 100, false);
                halocap::app::OcrResultWindow* probeWindow =
                    ocrWindows.empty() ? nullptr : ocrWindows.back().get();
                if (probeWindow != nullptr && probeWindow->Valid()) {
                    const HWND probeHandle = probeWindow->Handle();
                    visibleBeforeCapture = IsWindowVisible(probeHandle) != FALSE;
                    openOcr();
                    visibleDuringCapture = IsWindowVisible(probeHandle) != FALSE;
                    if (screenshotOverlay != nullptr) {
                        screenshotOverlay->Close();
                        screenshotOverlay.reset();
                    }
                    Sleep(150);
                    persistsOk = visibleBeforeCapture && visibleDuringCapture &&
                                 IsWindowVisible(probeHandle) != FALSE && probeWindow->Valid();
                }
                for (auto& window : ocrWindows) {
                    window->Close();
                }
                ocrWindows.clear();
                TrimProcessMemory();
                Sleep(150);
            }
            AddFlag(report, "APP_OCR_PERSISTS_CAPTURE", persistsOk);
            AddFlag(report, "APP_OCR_VISIBLE_BEFORE_CAPTURE", visibleBeforeCapture);
            AddFlag(report, "APP_OCR_VISIBLE_DURING_CAPTURE", visibleDuringCapture);

            // 功能判据：窗口起来、证明图写出、识别真的跑过；内存判据也进闸门——
            // 关掉覆盖层与结果窗口之后必须回到预算内（S4 的退出条件）。
            // 这条能拦住「又把某台设备/某个图集常驻起来」这类回归。
            ocrSelftestOk =
                windowOpen && recognized && proof && persistsOk && ocrRecognizeMs > 0.0 &&
                overlayMemoryOk;
            log.Info(ocrSelftestOk ? "ocr selftest: ok" : "ocr selftest: failed");
        } else {
            log.Error("ocr selftest: overlay did not open");
        }
        running = false;
    }

    bool settingsClickOk = true;
    if (!options.settingsClickSelftestPath.empty()) {
        settingsClickOk = false;
        core::AppSettings captured;
        bool saved = false;
        auto testWindow = std::make_unique<halocap::app::SettingsWindow>();
        const bool opened = testWindow->Create(
            nullptr, settings, facts, ResolveOcrModelDirectory(),
            [&](const core::AppSettings& updated, std::wstring&) {
                captured = updated;
                saved = true;
                return true;
            });
        if (opened) {
            Sleep(200);
            RECT windowRect{};
            GetWindowRect(testWindow->Handle(), &windowRect);
            const LRESULT dragHit = SendMessageW(
                testWindow->Handle(), WM_NCHITTEST, 0,
                MAKELPARAM(windowRect.left + 300, windowRect.top + 80));
            const bool draggable = dragHit == HTCAPTION;
            // 自检里的坐标是 DIP；鼠标消息要按当前 DPI 缩放折算成客户区像素。
            const double clickScale = testWindow->ScaleForTest() / 100.0;
            const auto click = [&](int xDip, int yDip) {
                const LPARAM point = MAKELPARAM(static_cast<int>(std::lround(xDip * clickScale)),
                                                static_cast<int>(std::lround(yDip * clickScale)));
                SendMessageW(testWindow->Handle(), WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(testWindow->Handle(), WM_LBUTTONUP, 0, point);
            };
            click(800, 241);   // 通用页「开机自启」开关（组标签下方第一行）
            const std::int64_t clickStart = QpcNowUs();
            click(100, 273);   // 切到「录屏」页
            click(793, 595);   // 倒计时选「5 秒」
            const double clickMs = static_cast<double>(QpcNowUs() - clickStart) / 1000.0;
            Sleep(120);

            halocap::platform::win32::DesktopImage desktop;
            const bool proof =
                halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                halocap::platform::win32::SaveBgraPng(options.settingsClickSelftestPath,
                                                      desktop.bgra.data(), desktop.width,
                                                      desktop.height, desktop.width * 4);
            click(796, 638);   // 保存
            Sleep(120);

            AddFlag(report, "APP_SETTINGS_CLICK_WINDOW", true);
            AddFlag(report, "APP_SETTINGS_CLICK_PROOF", proof);
            AddFlag(report, "APP_SETTINGS_DRAGGABLE", draggable);
            AddFlag(report, "APP_SETTINGS_CLICK_SAVED", saved);
            AddFlag(report, "APP_SETTINGS_CLICK_TOGGLED", captured.autoStart != settings.autoStart);
            IFileOpenDialog* folderProbe = nullptr;
            const bool folderDialogOk =
                SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&folderProbe)));
            if (folderProbe != nullptr) {
                folderProbe->Release();
            }
            AddFlag(report, "APP_SETTINGS_FOLDER_DIALOG", folderDialogOk);
            const bool countdownOk =
                captured.recording.countdown == core::RecordingCountdown::FiveSeconds;
            AddFlag(report, "APP_SETTINGS_COUNTDOWN", countdownOk);
            AddInt(report, "APP_SETTINGS_CLICK_RENDER_MS",
                   static_cast<long long>(std::lround(clickMs)));
            settingsClickOk = proof && saved && captured.autoStart != settings.autoStart &&
                             countdownOk && folderDialogOk && draggable;
            testWindow.reset();
        }
        log.Info(settingsClickOk ? "settings click selftest: ok"
                                 : "settings click selftest: failed");
        running = false;
    }

    // 热键自检：捕获是低级钩子驱动的，只能注入真按键来验。四道闸门各验一次：
    // 与别的动作冲突、裸键不安全、合法组合被接受、Esc 取消；再加一道「带着冲突保存」。
    bool hotkeySelftestOk = true;
    if (!options.hotkeySelftestPath.empty()) {
        hotkeySelftestOk = false;
        // WH_KEYBOARD_LL 的回调由装钩子的线程在自己的消息队列上执行。自检块跑在主线程、
        // 没有消息循环，所以注入之后必须自己抽队列，否则钩子一次都不会被调用——
        // 这一步漏掉时四个闸门全假绿（捕获态根本没退出，看起来像「按了没反应」）。
        const auto pump = [](int milliseconds) {
            const DWORD deadline = GetTickCount() + static_cast<DWORD>(milliseconds);
            MSG message;
            while (GetTickCount() < deadline) {
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(5);
            }
        };
        int sendFailures = 0;
        const auto press = [&](WORD virtualKey, bool down) {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = virtualKey;
            input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            if (SendInput(1, &input, sizeof(input)) != 1) {
                ++sendFailures;
            }
            // 每注入一个事件就抽一次队列：钩子里用 GetAsyncKeyState 读修饰键状态，
            // 一次塞六个事件再抽，轮到主键按下时那个状态可能还没跟上（实测会读成 0）。
            pump(15);
        };
        const auto combo = [&](WORD key, bool control, bool alt) {
            if (control) { press(VK_CONTROL, true); }
            if (alt) { press(VK_MENU, true); }
            press(key, true);
            press(key, false);
            if (alt) { press(VK_MENU, false); }
            if (control) { press(VK_CONTROL, false); }
            pump(60);
        };
        const auto grabProof = [&](const std::wstring& path) {
            halocap::platform::win32::DesktopImage desktop;
            return halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                   halocap::platform::win32::SaveBgraPng(path, desktop.bgra.data(), desktop.width,
                                                         desktop.height, desktop.width * 4);
        };

        core::AppSettings captured;
        bool saved = false;
        auto testWindow = std::make_unique<halocap::app::SettingsWindow>();
        const bool opened = testWindow->Create(
            nullptr, settings, facts, ResolveOcrModelDirectory(),
            [&](const core::AppSettings& updated, std::wstring&) {
                captured = updated;
                saved = true;
                return true;
            });
        bool conflictRejected = false;
        bool unsafeRejected = false;
        bool accepted = false;
        bool cancelled = false;
        bool proof = false;
        bool hookInstalled = false;
        int hookEvents = 0;
        std::string errorAfterConflict;
        if (opened) {
            testWindow->ShowPageForTest(1);
            SetForegroundWindow(testWindow->Handle());
            pump(200);

            // ① 按光圈那一把（默认 Ctrl+Alt+D）：拒绝、留在捕获态、原值不动。
            testWindow->BeginHotkeyCaptureForTest(1);
            hookInstalled = testWindow->HookInstalledForTest();
            combo(static_cast<WORD>(halocap::platform::win32::ToNativeVirtualKey(
                      settings.radialHotkey.key)),
                  (settings.radialHotkey.modifiers & core::kModControl) != 0,
                  (settings.radialHotkey.modifiers & core::kModAlt) != 0);
            hookEvents = testWindow->HookEventsForTest();
            errorAfterConflict = WideToUtf8(testWindow->ErrorForTest());
            conflictRejected =
                testWindow->CaptureIndexForTest() == 1 &&
                testWindow->SettingsForTest().screenshotHotkey == settings.screenshotHotkey &&
                testWindow->ErrorForTest().find(L"呼出光圈") != std::wstring::npos;
            proof = grabProof(options.hotkeySelftestPath);

            // ② 裸空格：不是安全的单键，同样拒绝且留在捕获态。
            press(VK_SPACE, true);
            press(VK_SPACE, false);
            pump(120);
            unsafeRejected = testWindow->CaptureIndexForTest() == 1 &&
                             testWindow->ErrorForTest().find(L"该单键不适合") != std::wstring::npos;

            // ③ Ctrl+Alt+K：接受，退出捕获态，横幅清掉。
            combo('K', true, true);
            accepted = testWindow->CaptureIndexForTest() == -1 &&
                       testWindow->ErrorForTest().empty() &&
                       testWindow->SettingsForTest().screenshotHotkey ==
                           core::HotkeySpec{ core::KeySymbol::K, core::kModControl | core::kModAlt };
            const core::HotkeySpec acceptedSpec = testWindow->SettingsForTest().screenshotHotkey;

            // ④ 再点同一行进入捕获，Esc 取消：值保持③的结果。
            testWindow->BeginHotkeyCaptureForTest(1);
            press(VK_ESCAPE, true);
            press(VK_ESCAPE, false);
            pump(120);
            cancelled = testWindow->CaptureIndexForTest() == -1 &&
                        testWindow->SettingsForTest().screenshotHotkey == acceptedSpec;
            testWindow.reset();
        }

        // ⑤ 文件里本来就带着两把相同的键（手改/迁移进来的）：保存必须被挡住，
        //    窗口不关、onSave 不被调用。捕获路径挡不住这种，只有保存闸门能挡。
        bool saveBlocked = false;
        bool saveClickBlocked = false;
        bool saveGateBlocked = false;
        int saveClickId = 0;
        int scalePercent = 0;
        bool saveHandlerCalled = false;
        core::AppSettings duplicated = settings;
        duplicated.screenshotHotkey = duplicated.radialHotkey;
        auto duplicateWindow = std::make_unique<halocap::app::SettingsWindow>();
        const bool duplicateOpened = duplicateWindow->Create(
            nullptr, duplicated, facts, ResolveOcrModelDirectory(),
            [&](const core::AppSettings&, std::wstring&) {
                saveHandlerCalled = true;
                return true;
            });
        if (duplicateOpened) {
            duplicateWindow->ShowPageForTest(1);
            pump(150);
            scalePercent = duplicateWindow->ScaleForTest();
            // 保存按钮在 DIP 里是 (754..838, 622..654)，取中心 (796, 638) 再乘缩放。
            // 直接用「窗口宽 - 120」在 150% 缩放下会落到按钮下面 3 px（实测点偏了）。
            const int clickX = 796 * scalePercent / 100;
            const int clickY = 638 * scalePercent / 100;
            saveClickId = duplicateWindow->HitTestForTest(clickX, clickY);
            const LPARAM point = MAKELPARAM(clickX, clickY);
            SendMessageW(duplicateWindow->Handle(), WM_LBUTTONDOWN, MK_LBUTTON, point);
            SendMessageW(duplicateWindow->Handle(), WM_LBUTTONUP, 0, point);
            pump(120);
            saveClickBlocked = !saveHandlerCalled && duplicateWindow->Valid() &&
                               duplicateWindow->ErrorForTest().find(L"相同的快捷键") !=
                                   std::wstring::npos;
            grabProof(options.hotkeySelftestPath + L".save-blocked.png");
            // 命中区与闸门分开判：再直接调一次 Save()，证明拦住的是闸门而不是坐标恰好没点上。
            saveHandlerCalled = false;
            duplicateWindow->SaveForTest();
            pump(80);
            saveGateBlocked = !saveHandlerCalled && duplicateWindow->Valid() &&
                              duplicateWindow->ErrorForTest().find(L"相同的快捷键") !=
                                  std::wstring::npos;
            saveBlocked = saveClickBlocked && saveGateBlocked;
            duplicateWindow.reset();
        }

        AddFlag(report, "APP_HOTKEY_WINDOW", opened);
        AddFlag(report, "APP_HOTKEY_PROOF", proof);
        AddFlag(report, "APP_HOTKEY_CONFLICT_REJECTED", conflictRejected);
        AddFlag(report, "APP_HOTKEY_UNSAFE_REJECTED", unsafeRejected);
        AddFlag(report, "APP_HOTKEY_ACCEPTED", accepted);
        AddFlag(report, "APP_HOTKEY_CANCELLED", cancelled);
        AddFlag(report, "APP_HOTKEY_SAVE_BLOCKED", saveBlocked);
        AddFlag(report, "APP_HOTKEY_SAVE_CLICK_BLOCKED", saveClickBlocked);
        AddFlag(report, "APP_HOTKEY_SAVE_GATE_BLOCKED", saveGateBlocked);
        AddInt(report, "APP_HOTKEY_SAVE_CLICK_ID", saveClickId);
        AddInt(report, "APP_HOTKEY_SCALE_PCT", scalePercent);
        AddFlag(report, "APP_HOTKEY_HOOK_INSTALLED", hookInstalled);
        AddInt(report, "APP_HOTKEY_HOOK_EVENTS", hookEvents);
        AddInt(report, "APP_HOTKEY_SEND_FAILURES", sendFailures);
        AddText(report, "APP_HOTKEY_ERROR_AFTER_CONFLICT",
                errorAfterConflict.empty() ? "none" : errorAfterConflict);
        hotkeySelftestOk = opened && proof && conflictRejected && unsafeRejected && accepted &&
                           cancelled && duplicateOpened && saveBlocked;
        log.Info(hotkeySelftestOk ? "hotkey selftest: ok" : "hotkey selftest: failed");
        running = false;
    }

    // 「改完真的能用」自检：走 app 自己的设置窗 → 真实保存回调（重新注册五把）→ 再按新键，
    // 看输入主机有没有把 token 交回来。--hotkey-selftest 只验到「设置里的值被改了」，
    // 这一条验的是改完之后按键真的会触发动作（用户报的就是这一段没生效）。
    bool hotkeyLiveOk = true;
    if (!options.hotkeyLiveSelftestPath.empty()) {
        hotkeyLiveOk = false;
        const core::AppSettings original = liveSettings;
        const auto pump = [] {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        };
        const auto press = [&](WORD virtualKey, bool down) {
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wVk = virtualKey;
            event.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            SendInput(1, &event, sizeof(event));
            pump();
            Sleep(20);
        };
        const auto drain = [&] {
            int token = 0;
            while (input.PollHotkey(token)) {
            }
        };
        const auto tapAndWait = [&](WORD virtualKey, int& tokenOut) {
            press(virtualKey, true);
            press(virtualKey, false);
            tokenOut = 0;
            for (int i = 0; i < 30; ++i) {
                input.PumpMessages();
                if (input.PollHotkey(tokenOut)) {
                    return true;
                }
                Sleep(10);
            }
            return false;
        };

        // 换成 F9：默认里没人用，也不和系统抢。
        constexpr WORD kNewKey = VK_F9;
        bool opened = false;
        bool captured = false;
        bool saved = false;
        bool delivered = false;
        bool restored = false;
        bool saveBlockedShown = false;
        bool swallowDelivered = false;
        int deliveredToken = 0;
        std::string errorShown;

        if (inputStarted) {
            openSettings();
            opened = settingsWindow != nullptr && settingsWindow->Valid();
            if (opened) {
                settingsWindow->ShowPageForTest(1);
                // 捕获中点保存必须给横幅。原来这里静默 return，用户会以为「已经保存了，
                // 只是快捷键没注册上」——正是这次报上来的现象。
                settingsWindow->BeginHotkeyCaptureForTest(1);
                settingsWindow->SaveForTest();
                pump();
                saveBlockedShown =
                    settingsWindow->CaptureIndexForTest() == 1 &&
                    settingsWindow->ErrorForTest().find(L"正在等待新的快捷键") !=
                        std::wstring::npos;
                // 同一行再点一次 = 取消捕获；再点一次进入干净捕获，准备改键。
                settingsWindow->BeginHotkeyCaptureForTest(1);
                settingsWindow->BeginHotkeyCaptureForTest(1);
                press(kNewKey, true);
                press(kNewKey, false);
                captured = settingsWindow->CaptureIndexForTest() == -1 &&
                           settingsWindow->SettingsForTest().screenshotHotkey ==
                               core::HotkeySpec{ core::KeySymbol::F9, 0 };
                settingsWindow->SaveForTest();
                pump();
                Sleep(250);
                errorShown = WideToUtf8(settingsWindow->ErrorForTest());
                // 保存成功才会关窗；关不掉说明闸门或注册把它拦住了。
                saved = liveSettings.screenshotHotkey ==
                            core::HotkeySpec{ core::KeySymbol::F9, 0 } &&
                        !settingsWindow->Valid();
                drain();
                delivered = tapAndWait(kNewKey, deliveredToken) &&
                            deliveredToken == kTokenScreenshot;
                // 物理按键会被拦截钩子吞掉（默认放过注入的事件），把注入也纳入拦截范围，
                // 才能测到真实路径：「吞掉之后动作必须由钩子补发」。这条就是单键热键
                // 明明注册成功却按了没反应的那个 bug。
                input.SetInterception({ core::HotkeySpec{ core::KeySymbol::F9, 0 } });
                input.SetInterceptionIncludesInjected(true);
                drain();
                int swallowedToken = 0;
                swallowDelivered = tapAndWait(kNewKey, swallowedToken) &&
                                   swallowedToken == kTokenScreenshot;
                input.SetInterceptionIncludesInjected(false);
                if (settingsWindow->Valid()) {
                    settingsWindow->Close();
                }
            }

            // 不管成没成，都把用户原来的设置和注册还原回去。
            liveSettings = original;
            const bool written = core::SaveSettingsTo(store, original);
            registerAllHotkeys(original, nullptr);
            const core::SettingsLoadResult reloaded = core::LoadSettingsFrom(store);
            restored = written && reloaded.settings.screenshotHotkey == original.screenshotHotkey;
            drain();
        }

        AddFlag(report, "APP_HOTKEY_LIVE_INPUT", inputStarted);
        AddFlag(report, "APP_HOTKEY_LIVE_OPEN", opened);
        AddFlag(report, "APP_HOTKEY_LIVE_CAPTURED", captured);
        AddFlag(report, "APP_HOTKEY_LIVE_SAVED", saved);
        AddFlag(report, "APP_HOTKEY_LIVE_DELIVERED", delivered);
        AddInt(report, "APP_HOTKEY_LIVE_TOKEN", deliveredToken);
        AddFlag(report, "APP_HOTKEY_LIVE_RESTORED", restored);
        AddFlag(report, "APP_HOTKEY_LIVE_SAVE_BLOCKED_HINT", saveBlockedShown);
        AddFlag(report, "APP_HOTKEY_LIVE_SWALLOWED_DELIVERS", swallowDelivered);
        AddText(report, "APP_HOTKEY_LIVE_ERROR", errorShown.empty() ? "none" : errorShown);
        hotkeyLiveOk = inputStarted && opened && captured && saved && delivered && restored &&
                       saveBlockedShown && swallowDelivered;
        log.Info(hotkeyLiveOk ? "hotkey live selftest: ok" : "hotkey live selftest: failed");
        running = false;
    }

    // OCR 搜索自检：桌面上真有什么字是不确定的，所以喂一份合成结果，把命中计数、
    // 双向回绕、无匹配、清空查询走一遍。高亮是画在父窗口上的，用 PrintWindow 印一张
    // 窗口位图数绿/蓝像素——这样「清了查询但框还留在图上」这种回归能被抓住。
    bool ocrSearchSelftestOk = true;
    if (!options.ocrSearchSelftestPath.empty()) {
        ocrSearchSelftestOk = false;
        constexpr int kImageWidth = 640;
        constexpr int kImageHeight = 320;
        std::vector<std::uint8_t> image(static_cast<std::size_t>(kImageWidth) * kImageHeight * 4,
                                        235);
        for (std::size_t i = 3; i < image.size(); i += 4) {
            image[i] = 255;
        }
        const char* const texts[] = {
            "HaloCap 截图工具", "HaloCap 支持贴图", "文字识别 halocap",
            "录屏与音频",       "HALOCAP 离线运行", "设置与快捷键",
            // 长行：逐行视图必须自动换行，不能用省略号把后半段截掉。
            "这是一段很长的识别结果内容，用来验证逐行视图会自动换行，而不会把后半段截掉。"
        };
        std::vector<core::ocr::OcrLine> lines;
        for (int i = 0; i < static_cast<int>(std::size(texts)); ++i) {
            core::ocr::OcrLine line;
            line.text = texts[i];
            line.box = core::Rect{ 20.0f, 20.0f + i * 46.0f, 420.0f, 52.0f + i * 46.0f };
            line.confidence = 0.9f;
            lines.push_back(line);
        }
        const core::ocr::OcrResult result = core::ocr::BuildResult(std::move(lines));

        struct HighlightCounts {
            int green = 0;
            int blue = 0;
        };
        const auto renderWindow = [](HWND window, core::IntRect preview, const std::wstring& path) {
            HighlightCounts counts;
            RECT client{};
            GetClientRect(window, &client);
            const int width = client.right;
            const int height = client.bottom;
            if (width <= 0 || height <= 0) {
                return counts;
            }
            HDC windowDc = GetDC(window);
            HDC memory = CreateCompatibleDC(windowDc);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(memory, bitmap);
            if (bitmap != nullptr && bits != nullptr && PrintWindow(window, memory, PW_CLIENTONLY)) {
                const auto* pixels = static_cast<const std::uint8_t*>(bits);
                // 只数预览图区域：「复制全部」按钮本来就是 #007AFF，会污染蓝色计数。
                const int previewLeft = std::max(0, preview.left);
                const int previewTop = std::max(0, preview.top);
                const int previewRight = std::min(width, preview.right);
                const int previewBottom = std::min(height, preview.bottom);
                for (int y = previewTop; y < previewBottom; ++y) {
                    for (int x = previewLeft; x < previewRight; ++x) {
                        const std::uint8_t* pixel =
                            pixels + (static_cast<std::size_t>(y) * width + x) * 4;
                        if (pixel[0] == 113 && pixel[1] == 204 && pixel[2] == 46) {
                            ++counts.green;
                        }
                        if (pixel[0] == 255 && pixel[1] == 122 && pixel[2] == 0) {
                            ++counts.blue;
                        }
                    }
                }
                if (!path.empty()) {
                    halocap::platform::win32::SaveBgraPng(path, pixels, width, height, width * 4);
                }
            }
            SelectObject(memory, old);
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            DeleteDC(memory);
            ReleaseDC(window, windowDc);
            return counts;
        };

        auto searchWindow = std::make_unique<halocap::app::OcrResultWindow>();
        halocap::app::OcrResultOptions searchOptions;
        searchOptions.showImage = true;
        searchOptions.skipRecognition = true;
        searchOptions.anchor = core::IntRect{ 200, 300, 900, 700 };
        const bool opened = searchWindow->Create(nullptr, image.data(), kImageWidth, kImageHeight,
                                                 searchOptions, halocap::app::OcrRunner{});
        if (opened) {
            searchWindow->PresentResultForTest(result, 0.0);
        }
        bool counterOk = false;
        bool wrapOk = false;
        bool caseOk = false;
        bool noneOk = false;
        bool clearOk = false;
        bool repaintOk = false;
        bool lineViewOk = false;
        bool lineNavOk = false;
        bool textWheelOk = false;
        bool blockViewOk = false;
        int blockCount = 0;
        bool viewStableOk = false;
        bool dragStableOk = false;
        bool editOk = false;
        bool copyModeOk = false;
        HighlightCounts highlighted;
        HighlightCounts cleared;
        if (opened) {
            Sleep(250);
            const auto status = [&] { return searchWindow->StatusForTest(); };

            // "halocap" 命中第 0、1、2、4 行：一行里出现两次也只算一处（命中单位是行）。
            searchWindow->SetQueryForTest(L"halocap");
            Sleep(150);
            counterOk = searchWindow->MatchesForTest().size() == 4 &&
                        searchWindow->MatchIndexForTest() == 0 && status() == L"1/4";

            searchWindow->StepMatchForTest(1);
            searchWindow->StepMatchForTest(1);
            searchWindow->StepMatchForTest(1);
            const bool forward = searchWindow->MatchIndexForTest() == 3 && status() == L"4/4";
            searchWindow->StepMatchForTest(1);
            const bool forwardWrap = searchWindow->MatchIndexForTest() == 0 && status() == L"1/4";
            searchWindow->StepMatchForTest(-1);
            wrapOk = forward && forwardWrap && searchWindow->MatchIndexForTest() == 3 &&
                     status() == L"4/4";

            searchWindow->SetQueryForTest(L"HALOCAP");
            Sleep(150);
            caseOk = searchWindow->MatchesForTest().size() == 4 &&
                     searchWindow->MatchIndexForTest() == 0 && status() == L"1/4";

            searchWindow->SetQueryForTest(L"文字");
            Sleep(150);
            const bool chinese = searchWindow->MatchesForTest().size() == 1 && status() == L"1/1";
            searchWindow->SetQueryForTest(L"这段文字不存在");
            Sleep(150);
            searchWindow->StepMatchForTest(1);
            noneOk = chinese && searchWindow->MatchesForTest().empty() &&
                     searchWindow->MatchIndexForTest() == 0 && status() == L"无匹配";

            searchWindow->SetQueryForTest(L"halocap");
            Sleep(200);
            const core::IntRect preview = searchWindow->PreviewRectForTest();
            highlighted = renderWindow(searchWindow->Handle(), preview,
                                       options.ocrSearchSelftestPath);
            searchWindow->SetQueryForTest(L"");
            Sleep(200);
            cleared = renderWindow(searchWindow->Handle(), preview,
                                   options.ocrSearchSelftestPath + L".cleared.png");
            repaintOk = highlighted.green > 0 && highlighted.blue > 0 && cleared.green == 0 &&
                        cleared.blue == 0;
            clearOk = searchWindow->MatchesForTest().empty() &&
                      status() == L"输入关键词";

            // 逐行视图：长行要换行显示（原来用 DT_END_ELLIPSIS 会被截掉）。
            RECT viewBefore{};
            GetWindowRect(searchWindow->Handle(), &viewBefore);
            searchWindow->SetLineViewForTest(true);
            UpdateWindow(searchWindow->Handle());
            Sleep(300);
            lineViewOk = searchWindow->LineViewForTest();
            // 逐行视图的键盘选择：第一次 ↓ 选中首行，再前进两行、Home/End 夹到首尾。
            searchWindow->MoveSelectionForTest(1);
            const bool navFirst = searchWindow->SelectedLineForTest() == 0;
            searchWindow->MoveSelectionForTest(2);
            const bool navDown = searchWindow->SelectedLineForTest() == 2;
            searchWindow->MoveSelectionForTest(-10);
            const bool navHome = searchWindow->SelectedLineForTest() == 0;
            searchWindow->MoveSelectionForTest(1000);
            const bool navEnd =
                searchWindow->SelectedLineForTest() ==
                std::min<int>(searchWindow->LineCountForTest(), 500) - 1;
            lineNavOk = navFirst && navDown && navHome && navEnd;
            AddFlag(report, "APP_OCR_SEARCH_LINE_NAV", lineNavOk);
            halocap::platform::win32::DesktopImage linesDesktop;
            // 证明图：普通层级窗口会被前台应用盖住，抓图前临时置顶，抓完恢复。
            const LONG_PTR lineStyle = GetWindowLongPtrW(searchWindow->Handle(), GWL_EXSTYLE);
            SetWindowLongPtrW(searchWindow->Handle(), GWL_EXSTYLE, lineStyle | WS_EX_TOPMOST);
            SetWindowPos(searchWindow->Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            UpdateWindow(searchWindow->Handle());
            Sleep(150);
            const bool linesShot = halocap::platform::win32::GrabPrimaryDesktop(linesDesktop);
            SetWindowLongPtrW(searchWindow->Handle(), GWL_EXSTYLE, lineStyle);
            SetWindowPos(searchWindow->Handle(), HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (linesShot) {
                halocap::platform::win32::SaveBgraPng(
                    options.ocrSearchSelftestPath + L".lines.png", linesDesktop.bgra.data(),
                    linesDesktop.width, linesDesktop.height, linesDesktop.width * 4);
            }
            // 切回文本视图：窗口左上角必须回到切换前的位置（搜索栏开合只改高度）。
            searchWindow->SetLineViewForTest(false);
            Sleep(200);
            RECT viewAfter{};
            GetWindowRect(searchWindow->Handle(), &viewAfter);
            const bool viewStable = viewBefore.left == viewAfter.left &&
                                    viewBefore.top == viewAfter.top;
            viewStableOk = viewStable;
            AddFlag(report, "APP_OCR_SEARCH_VIEW_STABLE", viewStable);

            // 版面视图：两段文字（中间空出明显行距）必须聚成两块，块首行带「块 N」标签。
            {
                std::vector<core::ocr::OcrLine> paragraphLines;
                const char* const paragraphTexts[] = { "第一段第一行", "第一段第二行",
                                                       "第二段" };
                const float paragraphTops[] = { 20.0f, 66.0f, 150.0f };
                for (int i = 0; i < 3; ++i) {
                    core::ocr::OcrLine line;
                    line.text = paragraphTexts[i];
                    line.box = core::Rect{ 20.0f, paragraphTops[i], 420.0f,
                                           paragraphTops[i] + 32.0f };
                    line.confidence = 0.9f;
                    paragraphLines.push_back(line);
                }
                const core::ocr::OcrResult paragraph =
                    core::ocr::BuildResult(std::move(paragraphLines));
                searchWindow->PresentResultForTest(paragraph, 0.0);
                const bool textViewBefore = !searchWindow->BlockViewForTest();
                searchWindow->SetBlockViewForTest(true);
                UpdateWindow(searchWindow->Handle());
                Sleep(250);
                blockCount = searchWindow->BlockCountForTest();
                blockViewOk = textViewBefore && searchWindow->BlockViewForTest() &&
                              searchWindow->LineViewForTest() && blockCount == 2 &&
                              searchWindow->LineCountForTest() == 3;
                halocap::platform::win32::DesktopImage blocksDesktop;
                if (halocap::platform::win32::GrabPrimaryDesktop(blocksDesktop)) {
                    halocap::platform::win32::SaveBgraPng(
                        options.ocrSearchSelftestPath + L".blocks.png",
                        blocksDesktop.bgra.data(), blocksDesktop.width,
                        blocksDesktop.height, blocksDesktop.width * 4);
                }
                searchWindow->SetBlockViewForTest(false);
                searchWindow->PresentResultForTest(result, 0.0);
                UpdateWindow(searchWindow->Handle());
                Sleep(150);
            }
            AddFlag(report, "APP_OCR_SEARCH_BLOCKVIEW", blockViewOk);
            AddInt(report, "APP_OCR_SEARCH_BLOCKS", blockCount);

            // 连续显示必须在滚轮下能滚动：多行结果 + 直接给 EDIT 发滚轮消息。
            {
                std::vector<core::ocr::OcrLine> many;
                for (int i = 0; i < 60; ++i) {
                    core::ocr::OcrLine line;
                    line.text = "第" + std::to_string(i) + "行测试内容";
                    line.box = core::Rect{ 20.0f, 20.0f + i * 20.0f, 420.0f,
                                           36.0f + i * 20.0f };
                    line.confidence = 0.9f;
                    many.push_back(line);
                }
                searchWindow->PresentResultForTest(
                    core::ocr::BuildResult(std::move(many)), 0.0);
                searchWindow->SetLineViewForTest(false);
                Sleep(100);
                const int before = searchWindow->TextFirstLineForTest();
                SendMessageW(searchWindow->TextHandleForTest(), WM_MOUSEWHEEL,
                             MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                             MAKELPARAM(10, 10));
                Sleep(80);
                const int after = searchWindow->TextFirstLineForTest();
                textWheelOk = before == 0 && after > before;
                searchWindow->PresentResultForTest(result, 0.0);
                UpdateWindow(searchWindow->Handle());
            }
            AddFlag(report, "APP_OCR_TEXT_WHEEL", textWheelOk);

            // 拖动之后点按钮不许复位：把窗口挪开，再切逐行/文本，位置必须保持挪后的。
            RECT dragged{};
            GetWindowRect(searchWindow->Handle(), &dragged);
            SetWindowPos(searchWindow->Handle(), nullptr, dragged.left + 60, dragged.top + 40, 0,
                         0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT moved{};
            GetWindowRect(searchWindow->Handle(), &moved);
            searchWindow->SetLineViewForTest(true);
            searchWindow->SetLineViewForTest(false);
            Sleep(100);
            RECT afterMove{};
            GetWindowRect(searchWindow->Handle(), &afterMove);
            dragStableOk = moved.left == dragged.left + 60 && moved.top == dragged.top + 40 &&
                           afterMove.left == moved.left && afterMove.top == moved.top;
            AddFlag(report, "APP_OCR_SEARCH_DRAG_STABLE", dragStableOk);

            // 文本视图可直接编辑：改完文本同步回去，纯文本与行数都要跟着变。
            searchWindow->SetEditedTextForTest(L"Halo 编辑\r\n第二行");
            searchWindow->SyncEditedTextForTest();
            editOk = searchWindow->TextEditableForTest() &&
                     searchWindow->PlainTextForTest() == L"Halo 编辑\r\n第二行" &&
                     searchWindow->LineCountForTest() == 2;
            AddFlag(report, "APP_OCR_SEARCH_EDITABLE", editOk);

            // 复制优先选区；切到逐行显示后复制全部仍是纯文本（不带行号），编辑内容不丢。
            // 剪贴板可能被其它程序短暂占用：复制和读取都带几次重试，避免把偶发占用当成回归。
            const auto copyUntil = [&](const std::wstring& expected) {
                for (int attempt = 0; attempt < 10; ++attempt) {
                    searchWindow->CopyAllForTest();
                    Sleep(40);
                    halocap::platform::win32::ClipboardReadResult clip;
                    if (halocap::platform::win32::ReadClipboard(&clip) && clip.hasText &&
                        clip.text == WideToUtf8(expected)) {
                        return true;
                    }
                }
                return false;
            };
            searchWindow->SetEditSelectionForTest(0, 4);
            const bool selectionCopyOk = copyUntil(L"Halo");
            searchWindow->SetEditSelectionForTest(0, 0);
            searchWindow->SetLineViewForTest(true);
            searchWindow->SetLineViewForTest(false);
            const bool editPreserved =
                searchWindow->PlainTextForTest() == L"Halo 编辑\r\n第二行";
            searchWindow->SetLineViewForTest(true);
            const bool plainCopyOk = copyUntil(L"Halo 编辑\r\n第二行");
            searchWindow->SetLineViewForTest(false);
            AddFlag(report, "APP_OCR_COPY_SELECTION", selectionCopyOk);
            AddFlag(report, "APP_OCR_EDIT_PRESERVED", editPreserved);
            AddFlag(report, "APP_OCR_COPY_PLAIN_IN_LINES", plainCopyOk);
            // 系统剪贴板被别的程序独占时读不到内容，这两项只能跳过，不能当成复制回归。
            halocap::platform::win32::ClipboardReadResult clipboardProbe;
            const bool clipboardUsable =
                halocap::platform::win32::ReadClipboard(&clipboardProbe);
            AddFlag(report, "APP_OCR_COPY_CLIPBOARD_USABLE", clipboardUsable);
            copyModeOk =
                editPreserved && (!clipboardUsable || (selectionCopyOk && plainCopyOk));
            searchWindow.reset();
        }

        AddFlag(report, "APP_OCR_SEARCH_WINDOW", opened);
        AddFlag(report, "APP_OCR_SEARCH_COUNTER", counterOk);
        AddFlag(report, "APP_OCR_SEARCH_WRAP", wrapOk);
        AddFlag(report, "APP_OCR_SEARCH_CASE", caseOk);
        AddFlag(report, "APP_OCR_SEARCH_NONE", noneOk);
        AddFlag(report, "APP_OCR_SEARCH_CLEAR", clearOk);
        AddFlag(report, "APP_OCR_SEARCH_REPAINT", repaintOk);
        AddFlag(report, "APP_OCR_SEARCH_LINEVIEW", lineViewOk);
        AddInt(report, "APP_OCR_SEARCH_GREEN_ON", highlighted.green);
        AddInt(report, "APP_OCR_SEARCH_BLUE_ON", highlighted.blue);
        AddInt(report, "APP_OCR_SEARCH_GREEN_OFF", cleared.green);
        AddInt(report, "APP_OCR_SEARCH_BLUE_OFF", cleared.blue);
        ocrSearchSelftestOk = opened && counterOk && wrapOk && caseOk && noneOk && clearOk &&
                              repaintOk && lineViewOk && lineNavOk && textWheelOk && blockViewOk && viewStableOk &&
                              dragStableOk && editOk && copyModeOk;
        log.Info(ocrSearchSelftestOk ? "ocr search selftest: ok" : "ocr search selftest: failed");
        running = false;
    }

    bool settingsSelftestOk = true;
    if (!options.settingsSelftestPath.empty()) {
        settingsSelftestOk = false;
        openSettings();
        if (settingsWindow != nullptr && settingsWindow->Valid() && options.settingsSelftestPage >= 0) {
            settingsWindow->ShowPageForTest(options.settingsSelftestPage);
            if (options.settingsSelftestScroll > 0) {
                settingsWindow->ScrollForTest(options.settingsSelftestScroll);
            }
        }
        Sleep(350);

        const bool opened = settingsWindow != nullptr && settingsWindow->Valid();
        bool proof = false;
        bool taskbarOk = false;
        bool notTopmost = false;
        if (opened) {
            // 设置是独立应用窗口：必须带 WS_EX_APPWINDOW（TOOLWINDOW 会让它从任务栏消失）。
            const LONG_PTR exStyle = GetWindowLongPtrW(settingsWindow->Handle(), GWL_EXSTYLE);
            taskbarOk = (exStyle & WS_EX_APPWINDOW) != 0 && (exStyle & WS_EX_TOOLWINDOW) == 0;
            // 层级和普通窗口一致：创建时不能置顶（先读样式，再为了抓图临时升到最上层）。
            notTopmost = (exStyle & WS_EX_TOPMOST) == 0;
            // 自检进程不在前台，普通窗口会被其他窗口盖住；只为拍到界面临时置顶（生产不置顶）。
            SetWindowPos(settingsWindow->Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            Sleep(160);
            halocap::platform::win32::DesktopImage desktop;
            proof = halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                    halocap::platform::win32::SaveBgraPng(
                        options.settingsSelftestPath, desktop.bgra.data(), desktop.width,
                        desktop.height, desktop.width * 4);
            // logo 证明图：与设置页标题栏/托盘/窗口图标同一套渲染（空闲 + 录制）。
            {
                const std::vector<std::uint8_t> logo =
                    halocap::platform::win32::RenderHalocapLogoBgra(96, false);
                const std::vector<std::uint8_t> logoRecording =
                    halocap::platform::win32::RenderHalocapLogoBgra(96, true);
                halocap::platform::win32::SaveBgraPng(options.settingsSelftestPath + L".logo.png",
                                                      logo.data(), 96, 96, 96 * 4);
                halocap::platform::win32::SaveBgraPng(
                    options.settingsSelftestPath + L".logo-rec.png", logoRecording.data(), 96, 96,
                    96 * 4);
            }
            settingsWindow->Close();
        }

        AddFlag(report, "APP_SETTINGS_WINDOW", opened);
        AddFlag(report, "APP_SETTINGS_PROOF", proof);
        AddFlag(report, "APP_SETTINGS_TASKBAR", taskbarOk);
        AddFlag(report, "APP_SETTINGS_NOT_TOPMOST", notTopmost);
        settingsSelftestOk = opened && proof && taskbarOk && notTopmost;
        running = false;
    }

    // 模型下载自检：真实下载官方模型，并在下载期间持续泵消息、统计进度消息条数。
    // 2.2.0 的回归是「每 64 KB 重绘一次」把消息队列塞满，窗口既看不到进度又像卡死；
    // 这里要求下载完成且进度消息不超过 102 条（每 1% 一条，三个文件共用计数）。
    bool ocrModelDownloadSelftestOk = true;
    if (!options.ocrModelDownloadSelftestPath.empty()) {
        ocrModelDownloadSelftestOk = false;
        openSettings();
        const bool opened = settingsWindow != nullptr && settingsWindow->Valid();
        bool finished = false;
        int updates = 0;
        int models = 0;
        bool proof = false;
        if (opened) {
            settingsWindow->ShowPageForTest(3);
            settingsWindow->BeginOcrModelDownloadForTest();
            const auto pump = [] {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            };
            const ULONGLONG start = GetTickCount64();
            while (GetTickCount64() - start < 180000) {
                pump();
                if (!settingsWindow->ModelDownloadBusyForTest()) {
                    finished = true;
                    break;
                }
                Sleep(15);
            }
            for (int i = 0; i < 20; ++i) {
                pump();
                Sleep(10);
            }
            updates = settingsWindow->ModelProgressUpdatesForTest();
            models = settingsWindow->ModelCountForTest();
            halocap::platform::win32::DesktopImage desktop;
            proof = halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                    halocap::platform::win32::SaveBgraPng(
                        options.ocrModelDownloadSelftestPath, desktop.bgra.data(),
                        desktop.width, desktop.height, desktop.width * 4);
            settingsWindow->Close();
        }
        AddFlag(report, "APP_OCR_MODEL_DOWNLOAD_WINDOW", opened);
        AddFlag(report, "APP_OCR_MODEL_DOWNLOAD_DONE", finished);
        AddInt(report, "APP_OCR_MODEL_DOWNLOAD_UPDATES", updates);
        AddInt(report, "APP_OCR_MODEL_DOWNLOAD_MODELS", models);
        AddFlag(report, "APP_OCR_MODEL_DOWNLOAD_PROOF", proof);
        ocrModelDownloadSelftestOk =
            opened && finished && updates > 0 && updates <= 102 && models >= 1 && proof;
        running = false;
    }

    bool pinSelftestOk = true;
    if (!options.pinSelftestPath.empty()) {
        pinSelftestOk = false;
        const std::size_t before = pins.size();

        // 先贴剪贴板里现有的内容（自检脚本可以预先放一张图），再放颜色文本贴第二张。
        pinClipboard();
        const std::size_t afterCurrent = pins.size();
        const bool wrote =
            halocap::platform::win32::WriteUnicodeTextToClipboard("#123456");
        if (wrote) {
            pinClipboard();
        }
        // 文字/颜色卡片是不透明的：合成后的 alpha 必须是 255，不能整张贴图变透明。
        bool pinOpaqueOk = true;
        if (wrote && pins.size() > afterCurrent) {
            const std::uint32_t pixel = pins.back()->CompositedPixelForTest(10, 10);
            pinOpaqueOk = static_cast<int>(pixel >> 24) == 255;
        }
        AddFlag(report, "APP_PIN_OPAQUE", pinOpaqueOk);

        // 半透明颜色：色块本身要透出桌面（alpha=128），信息条仍不透明。
        bool pinColorAlphaOk = false;
        if (halocap::platform::win32::WriteUnicodeTextToClipboard("#0EA5E980")) {
            const std::size_t beforeAlphaColor = pins.size();
            pinClipboard();
            Sleep(120);
            if (pins.size() > beforeAlphaColor) {
                const std::uint32_t swatch =
                    pins.back()->CompositedPixelForTest(10, 10);
                pinColorAlphaOk = static_cast<int>(swatch >> 24) == 128;
                AddInt(report, "APP_PIN_COLOR_SWATCH_ALPHA",
                       static_cast<int>(swatch >> 24));
            }
        }
        AddFlag(report, "APP_PIN_COLOR_ALPHA", pinColorAlphaOk);
        // 透明 PNG/DIB：贴图内容必须保留 alpha（1.x 用 Pbgra32 渲染）。
        bool pinAlphaOk = false;
        bool pinAlphaSkipped = false;
        {
            constexpr int kAlphaSize = 64;
            std::vector<std::uint8_t> transparent(
                static_cast<std::size_t>(kAlphaSize) * kAlphaSize * 4);
            for (std::size_t i = 0; i + 3 < transparent.size(); i += 4) {
                transparent[i + 0] = 0;
                transparent[i + 1] = 0;
                transparent[i + 2] = 255;
                transparent[i + 3] = 128;
            }
            const bool alphaWritten = halocap::platform::win32::CopyBgraToClipboard(
                nullptr, transparent.data(), kAlphaSize, kAlphaSize, kAlphaSize * 4);
            if (!alphaWritten) {
                pinAlphaSkipped = true;
                pinAlphaOk = true;
            } else {
                const std::size_t beforeAlpha = pins.size();
                pinClipboard();
                Sleep(120);
                if (pins.size() > beforeAlpha) {
                    const std::uint32_t pixel =
                        pins.back()->CompositedPixelForTest(kAlphaSize / 2, kAlphaSize / 2);
                    const int composedAlpha = static_cast<int>(pixel >> 24);
                    const int composedRed = static_cast<int>((pixel >> 16) & 0xFF);
                    pinAlphaOk = pins.back()->ContentHasAlphaForTest() &&
                                 pins.back()->ContentAlphaForTest(kAlphaSize / 2,
                                                                  kAlphaSize / 2) == 128 &&
                                 composedAlpha == 128 && composedRed >= 250;
                    AddInt(report, "APP_PIN_ALPHA_COMPOSED", composedAlpha);
                    AddInt(report, "APP_PIN_ALPHA_RED", composedRed);
                }
            }
        }
        AddFlag(report, "APP_PIN_ALPHA", pinAlphaOk);
        AddFlag(report, "APP_PIN_ALPHA_SKIPPED", pinAlphaSkipped);
        Sleep(350);

        // 把鼠标移到第一张贴图上，让悬停工具条出现在证明图里。
        if (!pins.empty() && pins.back()->Handle() != nullptr) {
            RECT pinRect{};
            GetWindowRect(pins.back()->Handle(), &pinRect);
            SetCursorPos((pinRect.left + pinRect.right) / 2, (pinRect.top + pinRect.bottom) / 2);
            RECT client{};
            GetClientRect(pins.back()->Handle(), &client);
            SendMessageW(pins.back()->Handle(), WM_MOUSEMOVE, 0,
                         MAKELPARAM((client.right - client.left) / 2,
                                    (client.bottom - client.top) / 2));
            Sleep(300);
        }

        // 贴图标注与菜单：空格进入标注 → 画一笔 → 右键菜单 → Esc 收起菜单。
        bool pinAnnotateOk = false;
        bool pinMenuOk = false;
        int pinOptionSlack = -1;
        char pinOptionText[96] = {};
        if (!pins.empty() && pins.back()->Handle() != nullptr) {
            HWND pinWindow = pins.back()->Handle();
            SendMessageW(pinWindow, WM_KEYDOWN, VK_SPACE, 0);
            const core::IntRect content = pins.back()->ContentRectForTest();
            const int cx = (content.left + content.right) / 2;
            const int cy = (content.top + content.bottom) / 2;
            SendMessageW(pinWindow, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(cx - 20, cy));
            SendMessageW(pinWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(cx + 20, cy));
            SendMessageW(pinWindow, WM_LBUTTONUP, 0, MAKELPARAM(cx + 20, cy));
            pinAnnotateOk = pins.back()->AnnotatingForTest() &&
                            !pins.back()->DocumentForTest().Items().empty();
            const core::IntRect panel = pins.back()->OptionBoundsForTest();
            pinOptionSlack = panel.right - pins.back()->OptionContentRightForTest();
            std::snprintf(pinOptionText, sizeof(pinOptionText), "%d..%d top=%d", panel.left,
                          panel.right, panel.top);
            SendMessageW(pinWindow, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(cx, cy));
            const bool menuOpened = pins.back()->MenuOpenForTest();
            Sleep(150);
            halocap::platform::win32::DesktopImage menuDesktop;
            halocap::platform::win32::GrabPrimaryDesktop(menuDesktop);
            halocap::platform::win32::SaveBgraPng(options.pinSelftestPath + L".menu.png",
                                                  menuDesktop.bgra.data(), menuDesktop.width,
                                                  menuDesktop.height, menuDesktop.width * 4);
            SendMessageW(pinWindow, WM_KEYDOWN, VK_ESCAPE, 0);
            pinMenuOk = menuOpened && !pins.back()->MenuOpenForTest();
            Sleep(120);
        }

        const bool pinned = pins.size() > afterCurrent;
        halocap::platform::win32::DesktopImage desktop;
        const bool proof =
            halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
            halocap::platform::win32::SaveBgraPng(options.pinSelftestPath, desktop.bgra.data(),
                                                  desktop.width, desktop.height,
                                                  desktop.width * 4);

        // 外描边关掉 = 只剩图像本身：窗口尺寸必须和内容完全一致（和屏幕融在一起）。
        bool pinPlainOk = false;
        if (!pins.empty() && pins.back()->Handle() != nullptr) {
            HWND pinWindow = pins.back()->Handle();
            SendMessageW(pinWindow, WM_KEYDOWN, VK_ESCAPE, 0);   // 退出标注，不关窗
            pins.back()->SetOutline(false);
            Sleep(150);
            const core::IntRect plainContent = pins.back()->ContentRectForTest();
            RECT plainWindow{};
            GetWindowRect(pinWindow, &plainWindow);
            pinPlainOk = plainContent.left == 0 && plainContent.top == 0 &&
                         static_cast<int>(plainWindow.right - plainWindow.left) ==
                             plainContent.right &&
                         static_cast<int>(plainWindow.bottom - plainWindow.top) ==
                             plainContent.bottom;
            halocap::platform::win32::DesktopImage plainDesktop;
            halocap::platform::win32::GrabPrimaryDesktop(plainDesktop);
            halocap::platform::win32::SaveBgraPng(options.pinSelftestPath + L".plain.png",
                                                  plainDesktop.bgra.data(),
                                                  plainDesktop.width, plainDesktop.height,
                                                  plainDesktop.width * 4);
            pins.back()->SetOutline(true);
        }

        AddFlag(report, "APP_PIN_CLIPBOARD", wrote);
        AddFlag(report, "APP_PIN_WINDOW", pinned);
        AddFlag(report, "APP_PIN_ANNOTATE", pinAnnotateOk);
        AddFlag(report, "APP_PIN_MENU", pinMenuOk);
        AddFlag(report, "APP_PIN_PLAIN", pinPlainOk);
        AddInt(report, "APP_PIN_OPTION_SLACK", pinOptionSlack);
        AddText(report, "APP_PIN_OPTIONBAR", pinOptionText);
        AddFlag(report, "APP_PIN_PROOF", proof);
        AddInt(report, "APP_PIN_COUNT", static_cast<long long>(pins.size() - before));

        pinSelftestOk = wrote && pinned && proof && pinAnnotateOk && pinMenuOk && pinPlainOk &&
                        pinAlphaOk && pinOpaqueOk && pinColorAlphaOk && pinOptionSlack >= 0 &&
                        pinOptionSlack <= 20;
        log.Info(pinSelftestOk ? "pin selftest: ok" : "pin selftest: failed");
        running = false;
    }

    bool radialClickOk = true;
    if (!options.radialClickSelftestPath.empty()) {
        radialClickOk = false;
        SetCursorPos(960, 540);
        halocap::app::RadialMenu menu;
        core::AppAction picked = core::AppAction::Screenshot;
        const bool opened = menu.Open(
            [&](core::AppAction action) { picked = action; }, core::MenuSize::Medium);
        if (opened) {
            const ULONGLONG start = GetTickCount64();
            while (GetTickCount64() - start < 300) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(10);
            }
            const HWND handle = static_cast<HWND>(menu.HandleForTest());
            SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(320, 210));
            Sleep(100);
            halocap::platform::win32::DesktopImage desktop;
            const bool proof =
                halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                halocap::platform::win32::SaveBgraPng(options.radialClickSelftestPath,
                                                      desktop.bgra.data(), desktop.width,
                                                      desktop.height, desktop.width * 4);
            AddFlag(report, "APP_RADIAL_CLICK_PROOF", proof);
            AddFlag(report, "APP_RADIAL_CLICK", picked == core::AppAction::Ocr);
            radialClickOk = proof && picked == core::AppAction::Ocr;
        }
        menu.Close();
        log.Info(radialClickOk ? "radial click selftest: ok" : "radial click selftest: failed");
        running = false;
    }

    bool windowPickerSelftestOk = true;
    if (!options.windowPickerSelftestPath.empty()) {
        windowPickerSelftestOk = false;
        POINT saved{};
        GetCursorPos(&saved);
        SetCursorPos(900, 500);
        windowPicker = std::make_unique<halocap::app::WindowPicker>();
        const bool opened = windowPicker->Open([](HWND) {});
        Sleep(400);
        halocap::platform::win32::DesktopImage desktop;
        const bool proof =
            halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
            halocap::platform::win32::SaveBgraPng(options.windowPickerSelftestPath,
                                                  desktop.bgra.data(), desktop.width,
                                                  desktop.height, desktop.width * 4);
        if (windowPicker != nullptr) {
            windowPicker->Close();
        }
        SetCursorPos(saved.x, saved.y);
        AddFlag(report, "APP_WINDOW_PICKER_WINDOW", opened);
        AddFlag(report, "APP_WINDOW_PICKER_PROOF", proof);
        windowPickerSelftestOk = opened && proof;
        log.Info(windowPickerSelftestOk ? "window picker selftest: ok"
                                        : "window picker selftest: failed");
        running = false;
    }

    bool radialSelftestOk = true;
    if (!options.radialSelftestPath.empty()) {
        radialSelftestOk = false;
        SetCursorPos(960, 540);
        openRadial();
        const ULONGLONG radialStart = GetTickCount64();
        while (GetTickCount64() - radialStart < 500) {
            MSG radialMessage{};
            while (PeekMessageW(&radialMessage, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&radialMessage);
                DispatchMessageW(&radialMessage);
            }
            Sleep(10);
        }

        const bool opened = radialMenu != nullptr && radialMenu->Active();
        if (opened) {
            SetCursorPos(960 + 120, 540);
            SendMessageW(static_cast<HWND>(radialMenu->HandleForTest()), WM_MOUSEMOVE, 0, 0);
            const ULONGLONG hoverStart = GetTickCount64();
            while (GetTickCount64() - hoverStart < 260) {
                MSG hoverMessage{};
                while (PeekMessageW(&hoverMessage, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&hoverMessage);
                    DispatchMessageW(&hoverMessage);
                }
                Sleep(10);
            }
        }
        // 已呼出时再按快捷键：旧光圈必须被替换，并在新的光标位置重开。
        // 不检查旧 HWND 是否销毁——Windows 会复用句柄，旧窗口销毁后新窗口可能拿到同一个值。
        // 新窗口中心跟着新光标走，本身就说明它是重新呼出的。
        SetCursorPos(1400, 700);
        openRadial();
        const ULONGLONG reopenStart = GetTickCount64();
        while (GetTickCount64() - reopenStart < 300) {
            MSG reopenMessage{};
            while (PeekMessageW(&reopenMessage, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&reopenMessage);
                DispatchMessageW(&reopenMessage);
            }
            Sleep(10);
        }
        bool reopenOk = false;
        if (radialMenu != nullptr && radialMenu->Active()) {
            RECT reopened{};
            GetWindowRect(static_cast<HWND>(radialMenu->HandleForTest()), &reopened);
            const int centerX = (reopened.left + reopened.right) / 2;
            const int centerY = (reopened.top + reopened.bottom) / 2;
            reopenOk = std::abs(centerX - 1400) <= 3 && std::abs(centerY - 700) <= 3;
        }
        AddFlag(report, "APP_RADIAL_REOPEN_AT_CURSOR", reopenOk);

        halocap::platform::win32::DesktopImage desktop;
        const bool proof =
            halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
            halocap::platform::win32::SaveBgraPng(options.radialSelftestPath, desktop.bgra.data(),
                                                  desktop.width, desktop.height,
                                                  desktop.width * 4);
        if (radialMenu != nullptr) {
            radialMenu->Close();
        }

        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        const bool cursorNormal =
            GetCursorInfo(&cursorInfo) &&
            cursorInfo.hCursor != LoadCursorW(nullptr, IDC_WAIT) &&
            cursorInfo.hCursor != LoadCursorW(nullptr, IDC_APPSTARTING);
        AddFlag(report, "APP_RADIAL_CURSOR_NORMAL", cursorNormal);
        AddFlag(report, "APP_RADIAL_WINDOW", opened);
        AddFlag(report, "APP_RADIAL_PROOF", proof);
        radialSelftestOk = opened && proof && cursorNormal && reopenOk;
        log.Info(radialSelftestOk ? "radial selftest: ok" : "radial selftest: failed");
        running = false;
    }

    // 录屏 UI 自检：边框/倒计时/浮层三件套，不碰采集与编码，只验几何、采集排除与按钮回路。
    bool recordingUiSelftestOk = true;
    if (!options.recordingUiSelftestPath.empty()) {
        recordingUiSelftestOk = false;
        const auto grabProof = [&](const std::wstring& path) {
            halocap::platform::win32::DesktopImage desktop;
            return halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                   halocap::platform::win32::SaveBgraPng(path, desktop.bgra.data(), desktop.width,
                                                         desktop.height, desktop.width * 4);
        };
        const auto pumpFor = [](int ms) {
            const ULONGLONG start = GetTickCount64();
            while (GetTickCount64() - start < static_cast<ULONGLONG>(ms)) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(10);
            }
        };

        const core::IntRect anchor{ 400, 300, 1040, 660 };

        // 1) 边框：精确贴在锚点上、点击穿透、不进采集；UpdateRect 跟着新矩形走。
        halocap::app::RecordingFrame frame;
        const bool frameShown = frame.Show(anchor);
        bool frameGeometryOk = false;
        bool frameExcluded = false;
        bool frameClickThrough = false;
        bool frameUpdateOk = false;
        if (frameShown) {
            const HWND handle = static_cast<HWND>(frame.HandleForTest());
            RECT rect{};
            GetWindowRect(handle, &rect);
            frameGeometryOk = rect.left == anchor.left && rect.top == anchor.top &&
                              rect.right == anchor.right && rect.bottom == anchor.bottom;
            DWORD affinity = 0;
            GetWindowDisplayAffinity(handle, &affinity);
            frameExcluded = (affinity & WDA_EXCLUDEFROMCAPTURE) != 0;
            frameClickThrough = (GetWindowLongW(handle, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
            const core::IntRect moved{ 420, 320, 900, 600 };
            frame.UpdateRect(moved);
            GetWindowRect(handle, &rect);
            frameUpdateOk = rect.left == moved.left && rect.top == moved.top &&
                            rect.right == moved.right && rect.bottom == moved.bottom;
            frame.UpdateRect(anchor);
        }
        const bool frameProof = grabProof(options.recordingUiSelftestPath + L".frame.png");
        frame.Close();

        // 2) 倒计时：卡片居中于锚点、不进采集；probe 首帧画好后回调一次。
        halocap::app::RecordingCountdown countdown;
        bool countdownCentered = false;
        bool countdownExcluded = false;
        bool countdownProof = false;
        bool probeCalled = false;
        const bool countdownRan = countdown.Show(3, anchor, [&](HWND handle) {
            probeCalled = true;
            RECT rect{};
            GetWindowRect(handle, &rect);
            const int centerX = (rect.left + rect.right) / 2;
            const int centerY = (rect.top + rect.bottom) / 2;
            countdownCentered = std::abs(centerX - (anchor.left + anchor.right) / 2) <= 2 &&
                                std::abs(centerY - (anchor.top + anchor.bottom) / 2) <= 2;
            DWORD affinity = 0;
            GetWindowDisplayAffinity(handle, &affinity);
            countdownExcluded = (affinity & WDA_EXCLUDEFROMCAPTURE) != 0;
            countdownProof = grabProof(options.recordingUiSelftestPath + L".countdown.png");
        });

        // Esc 取消：probe 里注入 Esc 按下，倒计时回路看到后必须返回 false（取消）。
        halocap::app::RecordingCountdown cancelCountdown;
        const bool cancelled = !cancelCountdown.Show(3, anchor, [&](HWND) {
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wVk = VK_ESCAPE;
            event.ki.dwFlags = 0;
            SendInput(1, &event, sizeof(event));
        });
        {
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wVk = VK_ESCAPE;
            event.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &event, sizeof(event));
        }

        // 3) 浮层：卡片底边 = 锚点顶 - 10（1.x 上方优先），水平居中。
        halocap::app::RecordingHud hud;
        const bool hudCreated = hud.Create(anchor);
        bool hudPlacementOk = false;
        bool hudCentered = false;
        if (hudCreated) {
            const int pad = hud.PadForTest();
            RECT rect{};
            GetWindowRect(static_cast<HWND>(hud.HandleForTest()), &rect);
            hudPlacementOk = std::abs((rect.bottom - pad) - (anchor.top - 10)) <= 2;
            hudCentered = std::abs((rect.left + rect.right) / 2 -
                                   (anchor.left + anchor.right) / 2) <= 2;
        }

        // 上方放不下时翻到下方：锚点贴着工作区顶，卡片顶边 = 锚点底 + 10。
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitorInfo);
        const core::IntRect topAnchor{ monitorInfo.rcWork.left + 100, monitorInfo.rcWork.top + 10,
                                       monitorInfo.rcWork.left + 740, monitorInfo.rcWork.top + 200 };
        halocap::app::RecordingHud belowHud;
        bool hudBelowOk = false;
        if (belowHud.Create(topAnchor)) {
            RECT rect{};
            GetWindowRect(static_cast<HWND>(belowHud.HandleForTest()), &rect);
            hudBelowOk = std::abs((rect.top + belowHud.PadForTest()) - (topAnchor.bottom + 10)) <= 2;
        }
        belowHud.Close();

        // 信息行 + 电平表画一帧留证据；再验暂停/停止按钮与停止后两键一起禁用。
        bool hudPauseOk = false;
        bool hudStopOk = false;
        bool hudStopDisables = false;
        bool hudProof = false;
        if (hudCreated) {
            halocap::app::RecordingHudState state;
            state.elapsedMs = 65000;
            state.info = L"640×360 · 30fps · 1.2 MB · 系统+麦";
            state.showSystemLevel = true;
            state.systemLevel = 0.6f;
            state.showMicLevel = true;
            state.micLevel = 0.3f;
            hud.Update(state);
            pumpFor(120);
            hudProof = grabProof(options.recordingUiSelftestPath + L".hud.png");

            const HWND handle = static_cast<HWND>(hud.HandleForTest());
            const auto click = [&](const RECT& rect) {
                const LPARAM point = MAKELPARAM((rect.left + rect.right) / 2,
                                                (rect.top + rect.bottom) / 2);
                SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(handle, WM_LBUTTONUP, 0, point);
            };
            click(hud.PauseRectForTest());
            hudPauseOk = hud.PollAction() == halocap::app::RecordingHud::Action::Pause;
            click(hud.StopRectForTest());
            hudStopOk = hud.PollAction() == halocap::app::RecordingHud::Action::Stop;
            click(hud.StopRectForTest());
            hudStopDisables = hud.PollAction() == halocap::app::RecordingHud::Action::None;
        }
        hud.Close();

        AddFlag(report, "APP_RECORDING_UI_FRAME", frameShown);
        AddFlag(report, "APP_RECORDING_UI_FRAME_GEOMETRY", frameGeometryOk);
        AddFlag(report, "APP_RECORDING_UI_FRAME_EXCLUDED", frameExcluded);
        AddFlag(report, "APP_RECORDING_UI_FRAME_CLICK_THROUGH", frameClickThrough);
        AddFlag(report, "APP_RECORDING_UI_FRAME_UPDATE", frameUpdateOk);
        AddFlag(report, "APP_RECORDING_UI_FRAME_PROOF", frameProof);
        AddFlag(report, "APP_RECORDING_UI_COUNTDOWN", countdownRan);
        AddFlag(report, "APP_RECORDING_UI_COUNTDOWN_PROBE", probeCalled);
        AddFlag(report, "APP_RECORDING_UI_COUNTDOWN_CENTERED", countdownCentered);
        AddFlag(report, "APP_RECORDING_UI_COUNTDOWN_EXCLUDED", countdownExcluded);
        AddFlag(report, "APP_RECORDING_UI_COUNTDOWN_PROOF", countdownProof);
        AddFlag(report, "APP_RECORDING_UI_COUNTDOWN_CANCEL", cancelled);
        AddFlag(report, "APP_RECORDING_UI_HUD", hudCreated);
        AddFlag(report, "APP_RECORDING_UI_HUD_PLACEMENT", hudPlacementOk);
        AddFlag(report, "APP_RECORDING_UI_HUD_CENTERED", hudCentered);
        AddFlag(report, "APP_RECORDING_UI_HUD_BELOW", hudBelowOk);
        AddFlag(report, "APP_RECORDING_UI_HUD_PAUSE", hudPauseOk);
        AddFlag(report, "APP_RECORDING_UI_HUD_STOP", hudStopOk);
        AddFlag(report, "APP_RECORDING_UI_HUD_STOP_DISABLES", hudStopDisables);
        AddFlag(report, "APP_RECORDING_UI_HUD_PROOF", hudProof);
        recordingUiSelftestOk = frameShown && frameGeometryOk && frameExcluded && frameClickThrough &&
                                frameUpdateOk && frameProof && countdownRan && probeCalled &&
                                countdownCentered && countdownExcluded && countdownProof &&
                                cancelled && hudCreated && hudPlacementOk && hudCentered &&
                                hudBelowOk && hudPauseOk && hudStopOk && hudStopDisables && hudProof;
        log.Info(recordingUiSelftestOk ? "recording ui selftest: ok"
                                       : "recording ui selftest: failed");
        running = false;
    }

    // 托盘菜单自检：自绘面板条目模型与 core 一致、点击/键盘/Esc/点外收起、留证据。
    bool trayMenuSelftestOk = true;
    if (!options.trayMenuSelftestPath.empty()) {
        trayMenuSelftestOk = false;
        const auto grabProof = [&](const std::wstring& path) {
            halocap::platform::win32::DesktopImage desktop;
            return halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                   halocap::platform::win32::SaveBgraPng(path, desktop.bgra.data(), desktop.width,
                                                         desktop.height, desktop.width * 4);
        };
        const auto pumpFor = [](int ms) {
            const ULONGLONG start = GetTickCount64();
            while (GetTickCount64() - start < static_cast<ULONGLONG>(ms)) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(10);
            }
        };

        core::TrayMenuState state;
        state.radialHotkey = "Ctrl+Alt+D";
        state.screenshotHotkey = "Ctrl+Alt+A";
        state.ocrHotkey = "Ctrl+Alt+S";
        state.recordHotkey = "Ctrl+Alt+R";
        state.pinHotkey = "Ctrl+Alt+Q";
        state.pinAvailable = true;
        state.autoStart = true;
        const std::vector<core::TrayMenuItem> expected = core::BuildTrayMenu(state);
        const std::wstring versionLabel =
            std::wstring(L"v") + Utf8ToWide(halocap::app::kHaloCapVersion);
        const POINT openPoint{ 500, 300 };

        SetCursorPos(openPoint.x, openPoint.y);
        halocap::app::TrayMenuWindow menu;
        const bool opened = menu.Open(state, versionLabel, openPoint);
        pumpFor(150);

        bool modelOk = false;
        bool geometryOk = false;
        if (opened) {
            modelOk = menu.ItemCountForTest() == static_cast<int>(expected.size());
            int lastBottom = 0;
            geometryOk = true;
            for (int i = 0; i < menu.ItemCountForTest() && i < static_cast<int>(expected.size());
                 ++i) {
                const RECT rect = menu.ItemRectForTest(i);
                if (expected[i].separator) {
                    geometryOk = geometryOk && rect.right == rect.left;
                } else {
                    geometryOk = geometryOk && rect.right > rect.left && rect.bottom > rect.top &&
                                 rect.top >= lastBottom;
                    lastBottom = rect.bottom;
                }
            }
        }
        const bool proof = grabProof(options.trayMenuSelftestPath);

        // 鼠标点击「文字识别」项 → 命令回到 Ocr，菜单收起。
        bool clickOk = false;
        bool clickClosed = false;
        if (opened) {
            int ocrIndex = -1;
            for (int i = 0; i < static_cast<int>(expected.size()); ++i) {
                if (expected[i].command == core::TrayCommand::Ocr) {
                    ocrIndex = i;
                    break;
                }
            }
            if (ocrIndex >= 0) {
                const RECT rect = menu.ItemRectForTest(ocrIndex);
                const HWND handle = static_cast<HWND>(menu.HandleForTest());
                const LPARAM point = MAKELPARAM((rect.left + rect.right) / 2,
                                                (rect.top + rect.bottom) / 2);
                SendMessageW(handle, WM_MOUSEMOVE, 0, point);
                SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(handle, WM_LBUTTONUP, 0, point);
                clickOk = menu.PollCommand() == core::TrayCommand::Ocr;
                clickClosed = !menu.Active();
            }
        }
        menu.Close();

        // 键盘：Down 选中第一项、Enter 触发；另一份菜单 Esc 收起且不产生成命令。
        bool keyboardOk = false;
        bool escOk = false;
        {
            halocap::app::TrayMenuWindow keyMenu;
            SetCursorPos(openPoint.x, openPoint.y);
            if (keyMenu.Open(state, versionLabel, openPoint)) {
                pumpFor(150);
                const HWND handle = static_cast<HWND>(keyMenu.HandleForTest());
                SendMessageW(handle, WM_KEYDOWN, VK_DOWN, 0);
                const int highlight = keyMenu.HighlightForTest();
                SendMessageW(handle, WM_KEYDOWN, VK_RETURN, 0);
                keyboardOk = highlight == 0 &&
                             keyMenu.PollCommand() == expected[0].command && !keyMenu.Active();
            }
            keyMenu.Close();

            halocap::app::TrayMenuWindow escMenu;
            SetCursorPos(openPoint.x, openPoint.y);
            if (escMenu.Open(state, versionLabel, openPoint)) {
                pumpFor(150);
                SendMessageW(static_cast<HWND>(escMenu.HandleForTest()), WM_KEYDOWN, VK_ESCAPE, 0);
                escOk = !escMenu.Active() && escMenu.PollCommand() == core::TrayCommand::None;
            }
            escMenu.Close();
        }

        // 点面板外（阴影留白里的 (2,2)）收起，不产生成命令。
        bool outsideOk = false;
        {
            halocap::app::TrayMenuWindow outsideMenu;
            SetCursorPos(openPoint.x, openPoint.y);
            if (outsideMenu.Open(state, versionLabel, openPoint)) {
                pumpFor(150);
                SendMessageW(static_cast<HWND>(outsideMenu.HandleForTest()), WM_LBUTTONDOWN,
                             MK_LBUTTON, MAKELPARAM(2, 2));
                outsideOk =
                    !outsideMenu.Active() && outsideMenu.PollCommand() == core::TrayCommand::None;
            }
            outsideMenu.Close();
        }

        AddFlag(report, "APP_TRAY_MENU_WINDOW", opened);
        AddFlag(report, "APP_TRAY_MENU_MODEL", modelOk);
        AddFlag(report, "APP_TRAY_MENU_GEOMETRY", geometryOk);
        AddFlag(report, "APP_TRAY_MENU_PROOF", proof);
        AddFlag(report, "APP_TRAY_MENU_CLICK", clickOk);
        AddFlag(report, "APP_TRAY_MENU_CLICK_CLOSED", clickClosed);
        AddFlag(report, "APP_TRAY_MENU_KEYBOARD", keyboardOk);
        AddFlag(report, "APP_TRAY_MENU_ESC", escOk);
        AddFlag(report, "APP_TRAY_MENU_OUTSIDE", outsideOk);
        trayMenuSelftestOk = opened && modelOk && geometryOk && proof && clickOk && clickClosed &&
                             keyboardOk && escOk && outsideOk;
        log.Info(trayMenuSelftestOk ? "tray menu selftest: ok" : "tray menu selftest: failed");
        running = false;
    }

    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        int token = 0;
        while (input.PollHotkey(token)) {
            ++actions;
            if (token == kTokenRecord) {
                toggleRecording();
                continue;
            }
            if (token == kTokenScreenshot) {
                openScreenshot();
                continue;
            }
            if (token == kTokenOcr) {
                openOcr();
                continue;
            }
            if (token == kTokenPin) {
                pinClipboard();
                continue;
            }
            if (token == kTokenRadial) {
                openRadial();
                continue;
            }
            log.Info("action: hotkey token=" + std::to_string(token));
        }

        switch (tray.PollClick()) {
        case Win32Tray::Click::Right: {
            // 1.x：右键先收掉旧菜单，再在光标处开一份新的自绘菜单。
            if (trayMenu != nullptr) {
                trayMenu->Close();
                trayMenu.reset();
            }
            trayMenu = std::make_unique<halocap::app::TrayMenuWindow>();
            POINT cursor{};
            GetCursorPos(&cursor);
            const std::wstring versionLabel =
                std::wstring(L"v") + Utf8ToWide(halocap::app::kHaloCapVersion);
            if (!trayMenu->Open(trayState, versionLabel, cursor)) {
                trayMenu.reset();
                log.Error("tray menu: open failed");
            }
            break;
        }
        case Win32Tray::Click::Left:
            ++actions;
            openRadial();
            break;
        case Win32Tray::Click::None:
            break;
        }

        if (trayMenu != nullptr) {
            if (const core::TrayCommand command = trayMenu->PollCommand();
                command != core::TrayCommand::None) {
                onAction(command);
            }
            if (!trayMenu->Active()) {
                trayMenu.reset();
            }
        }

        if (const core::TrayCommand command = tray.PollCommand();
            command != core::TrayCommand::None) {
            onAction(command);
        }

        // 点「已保存」气泡：在资源管理器里选中刚录好的文件。
        if (tray.PollBalloonClick() && !savedBalloonPath.empty()) {
            RevealInExplorer(Utf8ToWide(savedBalloonPath));
            savedBalloonPath.clear();
        }

        // OCR 结果窗口关闭后释放；引擎空闲 60 秒后卸载，回到空闲内存预算内。
        if (std::any_of(ocrWindows.begin(), ocrWindows.end(),
                        [](const std::unique_ptr<halocap::app::OcrResultWindow>& window) {
                            return !window->Valid();
                        })) {
            pruneOcrWindows();
        }
        if (settingsWindow != nullptr && settingsPageOverride >= 0) {
            settingsWindow->ShowPageForTest(settingsPageOverride);
            settingsPageOverride = -1;
        }
        if (settingsWindow != nullptr && !settingsWindow->Valid()) {
            settingsWindow.reset();
        }
        if (radialMenu != nullptr && !radialMenu->Active()) {
            radialMenu.reset();
        }
        if (windowPicker != nullptr && !windowPicker->Active()) {
            windowPicker.reset();
        }
        // 截图覆盖层：会话自己的窗口消息由上面的 PeekMessage 派发，这里只推进渲染。
        if (screenshotOverlay != nullptr) {
            screenshotOverlay->Pump();
            if (!screenshotOverlay->Active()) {
                screenshotOverlay.reset();
            }
        }

        for (std::size_t i = 0; i < pins.size();) {
            if (!pins[i]->Valid()) {
                pins.erase(pins.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }

        // 窗口录制跟随：与 1.x 的 HudRefreshInterval 同一个 250ms 节奏——关闭/最小化就
        // 收尾保存，挪动/改尺寸就更新边框与浮层锚点；信息行也在这一拍重建。
        if (recordingActive) {
            const std::int64_t nowMs = static_cast<std::int64_t>(GetTickCount64());
            if (nowMs - lastHudInfoMs >= 250) {
                lastHudInfoMs = nowMs;
                if (recordTargetWindow != nullptr) {
                    RECT visible{};
                    if (!recordWindowUsable(recordTargetWindow, &visible)) {
                        log.Info("recording: target window closed or minimized, stop and save");
                        core::RecordingStats windowStats;
                        const bool saved = recording.Stop(windowStats);
                        recordingActive = false;
                        const std::string savedPath = recording.OutputPath();
                        closeRecordingUi();
                        refreshTray();
                        if (saved) {
                            notifySaved(savedPath);
                        } else {
                            notify("录制失败，详见日志");
                        }
                        if (recordRequested) {
                            recordStopped = saved;
                            recordStats = windowStats;
                            running = false;
                        }
                    } else {
                        const core::IntRect live{ visible.left, visible.top, visible.right,
                                                  visible.bottom };
                        if (live.left != recordAnchor.left || live.top != recordAnchor.top ||
                            live.right != recordAnchor.right ||
                            live.bottom != recordAnchor.bottom) {
                            recordAnchor = live;
                            if (recordingFrame != nullptr) {
                                recordingFrame->UpdateRect(live);
                            }
                            if (recordingHud != nullptr) {
                                recordingHud->UpdateAnchor(live);
                            }
                        }
                    }
                }
                if (recordingActive) {
                    hudInfoText = buildHudInfo();
                }
            }
        }

        // 录制泵：与消息循环同一个线程、同一个 30 ms 节奏。采集侧丢设备时这里会返回
        // false，录制作废并给出提示，而不是让 app 卡住。
        if (recordingHud != nullptr) {
            halocap::app::RecordingHudState hudState;
            hudState.elapsedMs = recording.MediaTimeMs();
            hudState.paused = recording.Paused();
            hudState.info = hudInfoText;
            hudState.showSystemLevel = recording.SystemAudioActive();
            hudState.systemLevel = recording.SystemAudioLevel();
            hudState.showMicLevel = recording.MicrophoneActive();
            hudState.micLevel = recording.MicrophoneLevel();
            recordingHud->Update(hudState);
            switch (recordingHud->PollAction()) {
            case halocap::app::RecordingHud::Action::Pause:
                togglePause();
                break;
            case halocap::app::RecordingHud::Action::Stop:
                toggleRecording();
                break;
            default:
                break;
            }
        }

        if (recordingActive) {
            if (!recording.Pump(QpcNowUs())) {
                log.Error(std::string("recording: ended by capture (") +
                          recording.LastErrorKind() + ")");
                core::RecordingStats ignored;
                recording.Stop(ignored);
                recordingActive = false;
                closeRecordingUi();
                refreshTray();
                notify("录制中断：采集源丢失");
                if (recordRequested) {
                    running = false;
                }
            }
        }

        if (recordRequested && recordingActive &&
            static_cast<std::int64_t>(GetTickCount64()) >= recordDeadlineMs) {
            recordOutputWidth = recording.OutputWidth();
            recordOutputHeight = recording.OutputHeight();
            recordStopped = recording.Stop(recordStats);
            recordingActive = false;
            closeRecordingUi();
            refreshTray();
            log.Info(recordStopped ? "recording: stopped" : "recording: stop failed");
            running = false;
        }

        if (options.selftest) {
            const std::int64_t elapsedMs = static_cast<std::int64_t>(GetTickCount64()) - startMs;

            if (!sampledIdle && elapsedMs >= idleDeadlineMs) {
                idlePrivate = PrivateMb();
                idleWorkingSet = WorkingSetMb();
                sampledIdle = true;
                log.Info("selftest: idle sampled");
            }

            if (sampledIdle && !overlayReady && options.overlay &&
                elapsedMs >= idleDeadlineMs) {
                overlayReady = overlay.Initialize(log, overlayWidth, overlayHeight);
                overlayAtMs = static_cast<std::int64_t>(GetTickCount64());
                if (!overlayReady) {
                    log.Error("selftest: overlay failed to initialize");
                    running = false;
                }
            }

            if (overlayReady && overlayPrivate < 0.0 &&
                static_cast<std::int64_t>(GetTickCount64()) - overlayAtMs >= overlaySettleMs) {
                overlayPrivate = PrivateMb();
                overlayWorkingSet = WorkingSetMb();
                log.Info("selftest: overlay sampled");
                running = false;
            }

            if (!options.overlay && sampledIdle && overlayPrivate < 0.0) {
                running = false;
            }

            if (elapsedMs > hardDeadlineMs) {
                log.Error("selftest: hard deadline reached");
                running = false;
            }
        }

        // 30 ms 一醒：够处理托盘/热键，也不至于空转烧 CPU。
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    // ---- 5) 结论 ----
    const double budgetMb = static_cast<double>(core::kIdleMemoryBudgetBytes) / 1048576.0;
    const bool idleWithin = core::WithinIdleBudget(static_cast<std::int64_t>(idlePrivate * 1048576.0));
    const bool overlayWithin =
        overlayPrivate < 0.0 ||
        core::WithinIdleBudget(static_cast<std::int64_t>(overlayPrivate * 1048576.0));

    AddMb(report, "APP_IDLE_PRIVATE_MB", idlePrivate);
    AddMb(report, "APP_IDLE_WORKING_SET_MB", idleWorkingSet);
    AddMb(report, "APP_OVERLAY_PRIVATE_MB", overlayPrivate);
    AddMb(report, "APP_OVERLAY_WORKING_SET_MB", overlayWorkingSet);
    AddMb(report, "APP_BUDGET_MB", budgetMb);
    AddFlag(report, "APP_IDLE_UNDER_BUDGET", idleWithin);
    AddFlag(report, "APP_OVERLAY_UNDER_BUDGET", overlayWithin);
    AddInt(report, "APP_ACTIONS", actions);
    AddInt(report, "APP_ELAPSED_MS",
           static_cast<long long>(GetTickCount64()) - startMs);

    bool ok = idleWithin && overlayWithin;
    if (options.selftest) {
        ok = ok && sampledIdle && (!options.overlay || overlayPrivate >= 0.0);
    }

    if (recordRequested) {
        const std::int64_t bytes = recordStopped ? FileBytesUtf8(recording.OutputPath()) : -1;
        AddFlag(report, "APP_RECORD_STARTED", recordStarted);
        AddFlag(report, "APP_RECORD_STOPPED", recordStopped);
        AddInt(report, "APP_RECORD_FRAMES", recordStats.framesSubmitted);
        AddInt(report, "APP_RECORD_AUDIO_FRAMES", recordStats.audioFrames);
        AddText(report, "APP_RECORD_PATH", recording.OutputPath());
        AddInt(report, "APP_RECORD_BYTES", bytes);
        if (options.hasRecordRegion) {
            AddInt(report, "APP_RECORD_REGION_WIDTH", options.recordRegion.right -
                                                          options.recordRegion.left);
            AddInt(report, "APP_RECORD_REGION_HEIGHT", options.recordRegion.bottom -
                                                           options.recordRegion.top);
        }
        AddInt(report, "APP_RECORD_OUTPUT_WIDTH", recordOutputWidth);
        AddInt(report, "APP_RECORD_OUTPUT_HEIGHT", recordOutputHeight);
        // 无人值守录制的判据：起来了、停了、有帧、文件非空。
        ok = ok && recordStarted && recordStopped && recordStats.framesSubmitted > 0 && bytes > 0;
    }

    AddText(report, "APP_SELFTEST", options.selftest ? (ok ? "OK" : "FAILED") : "SKIPPED");

    AddFlag(report, "APP_SCREENSHOT_SELFTEST", screenshotSelftestOk);
    AddFlag(report, "APP_OCR_SELFTEST", ocrSelftestOk);
    AddFlag(report, "APP_SETTINGS_SELFTEST", settingsSelftestOk);
    AddFlag(report, "APP_SETTINGS_CLICK_SELFTEST", settingsClickOk);
    AddFlag(report, "APP_HOTKEY_SELFTEST", hotkeySelftestOk);
    AddFlag(report, "APP_HOTKEY_LIVE_SELFTEST", hotkeyLiveOk);
    AddFlag(report, "APP_OCR_SEARCH_SELFTEST", ocrSearchSelftestOk);
    AddFlag(report, "APP_OCR_MODEL_DOWNLOAD_SELFTEST", ocrModelDownloadSelftestOk);
    AddFlag(report, "APP_PIN_SELFTEST", pinSelftestOk);
    AddFlag(report, "APP_RADIAL_SELFTEST", radialSelftestOk);
    AddFlag(report, "APP_WINDOW_PICKER_SELFTEST", windowPickerSelftestOk);
    AddFlag(report, "APP_RADIAL_CLICK_SELFTEST", radialClickOk);
    AddFlag(report, "APP_RECORDING_UI_SELFTEST", recordingUiSelftestOk);
    AddFlag(report, "APP_TRAY_MENU_SELFTEST", trayMenuSelftestOk);
    AddFlag(report, "APP_INPUT_RAW_REGISTERED", input.RawRegisteredForTest());
    AddInt(report, "APP_INPUT_RAW_EVENTS", input.RawKeyboardEventsForTest());
    AddInt(report, "APP_INPUT_RAW_MESSAGES", input.RawMessagesForTest());
    AddInt(report, "APP_INPUT_RAW_HOTKEYS", input.RawHotkeyDeliveriesForTest());
    ok = ok && screenshotSelftestOk && ocrSelftestOk && settingsSelftestOk && pinSelftestOk &&
         radialSelftestOk && windowPickerSelftestOk && hotkeySelftestOk && ocrSearchSelftestOk &&
         hotkeyLiveOk && recordingUiSelftestOk && trayMenuSelftestOk;

    std::printf("%s", report.c_str());
    if (!options.reportPath.empty()) {
        if (!WriteReportFile(options.reportPath, report)) {
            std::printf("APP_REPORT_WRITE=FAILED lastError=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
        }
    }

    log.Info(ok ? "shutdown: clean" : "shutdown: failed");
    tray.Close();
    input.Close();
    MfH264Encoder::ShutdownFoundation();
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // GUI 子系统：双击启动不弹控制台窗口。带参数（命令行/自检）时挂到父控制台，
    // 让 --version、报告等输出仍然可见；不带参数时什么都不挂，纯后台驻留。
    if (argc > 1 && AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
        FILE* console = nullptr;
        freopen_s(&console, "CONOUT$", "w", stdout);
        freopen_s(&console, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // OCR worker 子进程：跑完就退，不碰单实例、托盘与任何 UI。
    if (argc >= 5 && std::string_view(argv[1]) == "--ocr-worker") {
        return halocap::app::RunOcrWorker(argv[2], argv[3], argv[4]);
    }

    const Options options = ParseOptions(argc, argv);
    if (options.help) {
        PrintUsage();
        return 0;
    }

    if (options.version) {
        std::printf("%s %s (%s)\n", halocap::app::kHaloCapProductName,
                    halocap::app::kHaloCapVersion, halocap::app::kHaloCapChannel);
        return 0;
    }

    if (!options.updateManifestPath.empty()) {
        const std::string json = ReadTextFileUtf8(options.updateManifestPath);
        if (json.empty()) {
            std::printf("UPDATE_CHECK=FAILED reason=manifest-missing\n");
            return 2;
        }

        core::UpdateManifest manifest;
        std::string error;
        if (!core::ParseUpdateManifest(json, &manifest, &error)) {
            std::printf("UPDATE_CHECK=FAILED reason=%s\n", error.c_str());
            return 2;
        }

        const bool available =
            core::IsNewerVersion(manifest.version, halocap::app::kHaloCapVersion);
        std::printf("UPDATE_CURRENT=%s\n", halocap::app::kHaloCapVersion);
        std::printf("UPDATE_LATEST=%s\n", manifest.version.c_str());
        std::printf("UPDATE_AVAILABLE=%d\n", available ? 1 : 0);
        std::printf("UPDATE_URL=%s\n", manifest.url.c_str());
        std::printf("UPDATE_SIZE=%lld\n", static_cast<long long>(manifest.size));
        return 0;
    }

    if (options.updateCheck) {
        // 与设置页「检查更新」同一条路：GitHub Releases 的 atom feed 里取最新 tag。
        // GUI 子系统下 stdout 不可靠，结果同时写 --report 文件，自检按文件读。
        std::string report;
        int exitCode = 0;
        std::string body;
        std::string error;
        bool fetched = false;
        if (!options.updateAtomPath.empty()) {
            body = ReadTextFileUtf8(options.updateAtomPath);
            fetched = !body.empty();
            if (!fetched) {
                error = "atom-file";
            }
        } else {
            fetched = halocap::platform::win32::HttpGetUtf8(
                L"https://github.com/Joshua-Zion/HaloCap/releases.atom", &body, &error);
        }
        std::string tag;
        std::string parseError;
        if (!fetched) {
            AddText(report, "UPDATE_CHECK", "FAILED");
            AddText(report, "REASON", error);
            exitCode = 2;
        } else if (!core::ParseLatestTagFromAtom(body, &tag, &parseError)) {
            AddText(report, "UPDATE_CHECK", "FAILED");
            AddText(report, "REASON", parseError);
            exitCode = 2;
        } else {
            AddText(report, "UPDATE_CHECK", "OK");
            AddText(report, "UPDATE_CURRENT", halocap::app::kHaloCapVersion);
            AddText(report, "UPDATE_LATEST", tag);
            AddInt(report, "UPDATE_AVAILABLE",
                   core::IsNewerVersion(tag, halocap::app::kHaloCapVersion) ? 1 : 0);
            if (!options.updateDownloadDir.empty()) {
                const std::wstring tagWide = Utf8ToWide(tag);
                std::error_code ignored;
                std::filesystem::create_directories(options.updateDownloadDir, ignored);
                const std::wstring file =
                    options.updateDownloadDir + L"\\HaloCap-" + tagWide + L"-win-x64.zip";
                const std::wstring url =
                    L"https://github.com/Joshua-Zion/HaloCap/releases/download/v" + tagWide +
                    L"/HaloCap-" + tagWide + L"-win-x64.zip";
                std::string downloadError;
                const bool downloaded = halocap::platform::win32::HttpDownloadToFile(
                    url, file, {}, &downloadError);
                AddFlag(report, "UPDATE_DOWNLOAD", downloaded);
                if (downloaded) {
                    AddText(report, "UPDATE_DOWNLOAD_PATH", WideToUtf8(file));
                    AddInt(report, "UPDATE_DOWNLOAD_BYTES",
                           FileBytesUtf8(WideToUtf8(file)));
                } else {
                    AddText(report, "UPDATE_DOWNLOAD_ERROR", downloadError);
                    exitCode = 2;
                }
            }
        }
        std::printf("%s", report.c_str());
        if (!options.reportPath.empty()) {
            WriteReportFile(options.reportPath, report);
        }
        return exitCode;
    }

    // 正常启动只允许一份；selftest 可能被连跑多次，跳过这条。
    HANDLE instance = nullptr;
    if (!options.selftest) {
        instance = CreateMutexW(nullptr, FALSE, L"HaloCap2.SingleInstance");
        if (instance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
            std::printf("HaloCap is already running.\n");
            CloseHandle(instance);
            return 0;
        }
    }

    const int result = Run(options);

    if (instance != nullptr) {
        CloseHandle(instance);
    }
    return result;
}
