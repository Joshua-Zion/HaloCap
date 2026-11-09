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
#include "ocr_result_window.h"
#include "ocr_worker.h"
#include "ort_ocr_engine.h"
#include "pin_window.h"
#include "radial_menu.h"
#include "recording_hud.h"
#include "screenshot_overlay.h"
#include "settings_window.h"
#include "update.h"
#include "version.h"
#include "wasapi_audio_source.h"
#include "win32_autostart.h"
#include "win32_capability.h"
#include "win32_clipboard.h"
#include "win32_input_host.h"
#include "win32_store.h"
#include "win32_surface_window.h"
#include "win32_wgc_capture.h"
#include "window_picker.h"

#include <shobjidl.h>
#include "win32_tray.h"

#include <windows.h>

#include <psapi.h>

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

    // 交付：--version 打印版本；--check-update 读一份清单并比较版本。
    bool version = false;
    std::wstring updateManifestPath;
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
        } else if (argument == "--settings-page" && i + 1 < argc) {
            options.settingsSelftestPage = std::atoi(argv[++i]);
        } else if (argument == "--radial-click-selftest" && i + 1 < argc) {
            options.radialClickSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--window-picker-selftest" && i + 1 < argc) {
            options.windowPickerSelftestPath = Utf8ToWide(argv[++i]);
        } else if (argument == "--version") {
            options.version = true;
        } else if (argument == "--check-update" && i + 1 < argc) {
            options.updateManifestPath = Utf8ToWide(argv[++i]);
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
        "  --version                   print the delivery version and exit\n"
        "  --check-update PATH         compare this build with an update manifest\n"
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

    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) {
        return {};
    }
    return std::wstring(profile) + L"\\Videos\\Captures";
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
        options.outputIndex = 0;
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
    const core::AppSettings& settings = liveSettings;

    AddFlag(report, "APP_SETTINGS_FILE_EXISTS", loaded.fileExists);
    AddFlag(report, "APP_SETTINGS_SAVED", settingsSaved);
    AddInt(report, "APP_SETTINGS_MIGRATED", static_cast<long long>(loaded.report.migratedKeys.size()));
    AddInt(report, "APP_SETTINGS_IGNORED", static_cast<long long>(loaded.report.ignoredKeys.size()));
    AddInt(report, "APP_SETTINGS_REPAIRED", static_cast<long long>(loaded.report.repairedKeys.size()));
    AddInt(report, "APP_SETTINGS_DROPPED", static_cast<long long>(loaded.report.droppedKeys.size()));

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
    std::unique_ptr<halocap::app::WindowPicker> windowPicker;
    std::unique_ptr<halocap::app::ScreenshotOverlay> screenshotOverlay;
    std::vector<std::unique_ptr<halocap::app::PinWindow>> pins;
    bool recordStarted = false;
    bool recordStopped = false;
    core::RecordingStats recordStats;

    auto refreshTray = [&]() {
        trayState.recording = recordingActive;
        trayState.recordingPaused = recordingActive && recording.Paused();
        if (trayIcon) {
            tray.Apply(trayState);
        }
    };

    const auto beginRecording = [&](core::NativeHandle captureWindow, core::IntRect cropRegion) {
        if (settings.recording.countdown != core::RecordingCountdown::Off) {
            halocap::app::RecordingCountdown countdown;
            if (!countdown.Show(settings.recording.CountdownSeconds())) {
                log.Info("recording: cancelled during countdown");
                tray.Notify("HaloCap", "已取消录制");
                return;
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
            refreshTray();
            recordingHud = std::make_unique<halocap::app::RecordingHud>();
            if (!recordingHud->Create()) {
                recordingHud.reset();
            }
            log.Info("recording: started -> " + recording.OutputPath());
            tray.Notify("HaloCap", "开始录制");
        } else {
            log.Error("recording: start failed (" +
                      (overlayReady ? kind : std::string("overlay")) + ")");
            tray.Notify("HaloCap", "无法开始录制");
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
                    tray.Notify("HaloCap", "无法选择录制窗口");
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
                overlayOptions.onRegion = [&](core::IntRect rect) {
                    beginRecording(nullptr, rect);
                };
                if (!screenshotOverlay->Open(overlayOptions)) {
                    screenshotOverlay.reset();
                    tray.Notify("HaloCap", "无法选择录制区域");
                }
                return;
            }
            beginRecording(nullptr, core::IntRect{});
            return;
        }

        core::RecordingStats stats;
        const bool stopped = recording.Stop(stats);
        recordingActive = false;
        recordingHud.reset();
        refreshTray();
        log.Info(stopped ? "recording: stopped" : "recording: stop failed");
        if (stopped) {
            tray.Notify("HaloCap", "已保存：" + recording.OutputPath());
        } else {
            tray.Notify("HaloCap", "录制失败，详见日志");
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
        if (!overlayReady) {
            overlayReady = overlay.Initialize(log, overlayWidth, overlayHeight);
        }
        std::string kind;
        recordStarted = overlayReady &&
                        recording.Start(overlay.Backend(), settings, log, kind,
                                        options.recordWindow ? GetForegroundWindow() : nullptr,
                                        options.hasRecordRegion ? options.recordRegion
                                                                : core::IntRect{},
                                        !options.recordWindow, settings.recording.recordCursor);
        if (recordStarted) {
            recordingActive = true;
            refreshTray();
            recordingHud = std::make_unique<halocap::app::RecordingHud>();
            if (!recordingHud->Create()) {
                recordingHud.reset();
            }
            recordDeadlineMs =
                static_cast<std::int64_t>(GetTickCount64()) + options.recordSeconds * 1000;
            log.Info("recording: started (--record-seconds) -> " + recording.OutputPath());
        } else {
            log.Error("recording: start failed (" +
                      (overlayReady ? kind : std::string("overlay")) + ")");
            running = false;
        }
    }


    // OCR：推理放在一次性的 worker 子进程里，主进程不加载 ORT。
    std::unique_ptr<halocap::app::OcrResultWindow> ocrWindow;
    std::unique_ptr<halocap::app::SettingsWindow> settingsWindow;
    int settingsPageOverride = options.settingsSelftestPage;
    std::unique_ptr<halocap::app::RadialMenu> radialMenu;
    double ocrRecognizeMs = 0.0;
    std::string ocrWorkerEngine = "pp";
    std::function<void(const std::uint8_t*, int, int, int, int)> ocrRunner;

    const auto openScreenshot = [&]() {
        if (screenshotOverlay != nullptr && screenshotOverlay->Active()) {
            return;
        }
        screenshotOverlay = std::make_unique<halocap::app::ScreenshotOverlay>();
        halocap::app::ScreenshotOverlayOptions overlayOptions;
        overlayOptions.jpeg = settings.screenshotFormat == core::ScreenshotFormat::Jpeg;
        overlayOptions.onPin = [&](const std::uint8_t* bgra, int width, int height, int x, int y) {
            auto pin = std::make_unique<halocap::app::PinWindow>();
            if (pin->Create(bgra, width, height, x, y)) {
                pin->SetOcrHandler([&ocrRunner](const std::uint8_t* image, int imageWidth,
                                                int imageHeight) {
                    if (ocrRunner) {
                        ocrRunner(image, imageWidth, imageHeight, 0, 0);
                    }
                });
                pins.push_back(std::move(pin));
            }
        };

        if (!screenshotOverlay->Open(overlayOptions)) {
            log.Error(std::string("screenshot: overlay failed (") +
                      screenshotOverlay->LastErrorKind() + ")");
            screenshotOverlay.reset();
            tray.Notify("HaloCap", "截图失败：无法获取屏幕");
            return;
        }
        log.Info("screenshot: overlay opened");
    };

    const auto runOcr = [&](const std::uint8_t* bgra, int width, int height, int, int) {
        wchar_t executable[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) {
            log.Error("ocr: cannot resolve own path");
            return;
        }

        wchar_t temporary[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, temporary) == 0) {
            log.Error("ocr: no temp directory");
            return;
        }
        const std::filesystem::path directory =
            std::filesystem::path(temporary) / "HaloCap2";
        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);

        const std::wstring stem = L"ocr-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64());
        const std::wstring inputPath = (directory / (stem + L".raw")).wstring();
        const std::wstring outputPath = (directory / (stem + L".txt")).wstring();

        const std::string inputUtf8 = WideToUtf8(inputPath);
        const std::string outputUtf8 = WideToUtf8(outputPath);
        const std::string engineName =
            settings.ocr.engine == core::OcrEngine::Native ? "winrt" : "pp";
        ocrWorkerEngine = engineName;

        // 子进程靠环境变量找模型；如果 app 自己找得到，就把它传下去。
        if (const std::string models = ResolveOcrModelDirectory(); !models.empty()) {
            SetEnvironmentVariableW(L"HALOCAP_OCR_MODELS", Utf8ToWide(models).c_str());
        }

        if (!halocap::app::WriteOcrWorkerInput(inputUtf8, bgra, width, height)) {
            log.Error("ocr: cannot write worker input");
            return;
        }

        std::string error;
        const std::int64_t start = QpcNowUs();
        const bool spawned = halocap::app::SpawnOcrWorker(WideToUtf8(executable), inputUtf8,
                                                          outputUtf8, engineName, &error);
        halocap::app::OcrWorkerResult worker;
        const bool parsed = spawned && halocap::app::ReadOcrWorkerOutput(outputUtf8, &worker);
        ocrRecognizeMs = static_cast<double>(QpcNowUs() - start) / 1000.0;

        DeleteFileW(inputPath.c_str());
        DeleteFileW(outputPath.c_str());

        if (!parsed) {
            const std::string reason = worker.error.empty() ? error : worker.error;
            log.Error("ocr: worker failed (" + reason + ")");
            tray.Notify("HaloCap", "OCR 失败，详见日志");
            return;
        }

        const core::ocr::OcrResult result = core::ocr::BuildResult(std::move(worker.lines));
        log.Info("ocr: " + std::to_string(result.lines.size()) + " lines in " +
                 std::to_string(static_cast<int>(ocrRecognizeMs)) + " ms, engine=" + engineName);

        if (ocrWindow != nullptr) {
            ocrWindow->Close();
        }
        ocrWindow = std::make_unique<halocap::app::OcrResultWindow>();
        if (!ocrWindow->Create(nullptr, bgra, width, height, result)) {
            ocrWindow.reset();
            log.Error("ocr: result window failed");
        }
        if (result.lines.empty()) {
            tray.Notify("HaloCap", "OCR 没有识别到文字");
        }
    };

    ocrRunner = runOcr;

    const auto openOcr = [&]() {
        if (screenshotOverlay != nullptr && screenshotOverlay->Active()) {
            return;
        }
        screenshotOverlay = std::make_unique<halocap::app::ScreenshotOverlay>();
        halocap::app::ScreenshotOverlayOptions overlayOptions;
        overlayOptions.mode = halocap::app::OverlayMode::Ocr;
        overlayOptions.onOcr = runOcr;

        if (!screenshotOverlay->Open(overlayOptions)) {
            log.Error(std::string("ocr: overlay failed (") +
                      screenshotOverlay->LastErrorKind() + ")");
            screenshotOverlay.reset();
            tray.Notify("HaloCap", "OCR 无法获取屏幕");
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
                tray.Notify("HaloCap", "设置已保存（录制参数下次启动生效）");
                return true;
            });
        if (!created) {
            settingsWindow.reset();
            log.Error("settings: window failed");
        }
    };

    // 剪贴板贴图：读取 → core 决策 → 图片/卡片 → 分层贴图窗口。
    const auto pinClipboard = [&]() {
        halocap::platform::win32::ClipboardReadResult data;
        if (!halocap::platform::win32::ReadClipboard(&data)) {
            log.Error("pin: cannot read clipboard");
            tray.Notify("HaloCap", "无法读取剪贴板");
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
            tray.Notify("HaloCap", "剪贴板里没有可贴图的内容");
            return;
        }

        core::clipboard::PinPlanner planner;
        struct Item {
            halocap::platform::win32::ClipboardImage image;
        };
        std::vector<Item> items;

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
            const auto plans = planner.PlanFiles(snapshot, {}, loader);
            for (const auto& plan : plans) {
                if (plan.kind == core::clipboard::PinKind::FileImage) {
                    items.push_back(Item{ std::move(loaded[plan.text]) });
                    planner.MarkPasted(plan);
                } else {
                    halocap::platform::win32::ClipboardImage card;
                    if (halocap::platform::win32::RenderTextCard(plan.text, 1.0, &card)) {
                        items.push_back(Item{ std::move(card) });
                    }
                }
            }
        } else {
            const auto plans = planner.PlanContent(snapshot);
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
                            180 + offset, 160 + offset)) {
                pins.push_back(std::move(pin));
                ++created;
                offset += 28;
            }
        }

        log.Info("pin: " + std::to_string(created) + " item(s) from clipboard");
        if (created == 0) {
            tray.Notify("HaloCap", "贴图失败");
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
        if (radialMenu != nullptr && radialMenu->Active()) {
            return;
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
            //    自检没有消息循环，脏帧得自己 Pump 一次才会 Present。
            sendMouse(WM_MOUSEMOVE, baseX + 500, baseY + 500);
            screenshotOverlay->Pump();
            Sleep(150);
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
            // 形状工具：选项栏应当出现（8 色 + 分隔 + 矩形/椭圆/空心/实心）。
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
                                   screenshotOverlay->OptionButtonCountForTest() == 13;
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
            screenshotOverlay->Pump();
            Sleep(250);
            const double screenshotMb = PrivateMb();

            const bool proof = screenshotOverlay->GrabDesktopToPng(options.screenshotSelftestPath);
            std::string savedPath;
            const bool saved = screenshotOverlay->SaveSelection(&savedPath);
            const bool pinned = screenshotOverlay->PinSelection();
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
            AddFlag(report, "APP_SCREENSHOT_ANNOTATE", annotated);
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
                                   annotated && toolbarOk && optionsOk && clickShapeOk;
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
            screenshotOverlay->DebugSelect(core::IntRect{ 100, 100, 1000, 620 });
            screenshotOverlay->Pump();
            const double beforeOcrMb = PrivateMb();

            // 与截图自检同一条真实消息路径：WM_APP+4 在 OCR 模式里就是「识别」。
            SendMessageW(static_cast<HWND>(screenshotOverlay->WindowHandle()), WM_APP + 4, 0, 0);

            const bool windowOpen = ocrWindow != nullptr && ocrWindow->Valid();
            ocrSelftestLines = static_cast<int>(ocrWindow != nullptr ? 1 : 0);
            Sleep(300);
            const bool proof = screenshotOverlay->GrabDesktopToPng(options.ocrSelftestPath);
            const double ocrMb = PrivateMb();
            AddFlag(report, "APP_OCR_OVERLAY", true);
            AddFlag(report, "APP_OCR_WINDOW", windowOpen);
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
            ocrWindow.reset();
            TrimProcessMemory();
            Sleep(200);
            const double idleMb = PrivateMb();
            AddMb(report, "APP_OCR_AFTER_RELEASE_MB", idleMb);

            // 再关覆盖层：ScreenshotOverlay 自己建了一台 D3D/D2D 设备，与预热的那台
            // （OverlayStack，只服务录制）是两台。不分开量就分不清「模型残留」与「设备保留」。
            screenshotOverlay->Close();
            screenshotOverlay.reset();
            TrimProcessMemory();
            Sleep(400);
            // 多屏之后这一块是整块虚拟桌面的 D3D 纹理，驱动侧回收是异步的：再等一轮再压一次。
            TrimProcessMemory();
            Sleep(200);
            const double afterOverlayMb = PrivateMb();
            AddMb(report, "APP_OCR_AFTER_OVERLAY_CLOSE_MB", afterOverlayMb);
            AddFlag(report, "APP_OVERLAY_PREWARMED", overlayReady);
            AddFlag(report, "APP_OCR_WORKER", true);
            AddText(report, "APP_OCR_ENGINE", ocrWorkerEngine.c_str());
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

            // 功能判据：窗口起来、证明图写出、识别真的跑过；内存判据也进闸门——
            // 关掉覆盖层与结果窗口之后必须回到预算内（S4 的退出条件）。
            // 这条能拦住「又把某台设备/某个图集常驻起来」这类回归。
            ocrSelftestOk = windowOpen && proof && ocrRecognizeMs > 0.0 && overlayMemoryOk;
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
            const auto click = [&](int x, int y) {
                const LPARAM point = MAKELPARAM(x, y);
                SendMessageW(testWindow->Handle(), WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(testWindow->Handle(), WM_LBUTTONUP, 0, point);
            };
            click(800, 241);   // 通用页「开机自启」开关（组标签下方第一行）
            const std::int64_t clickStart = QpcNowUs();
            click(100, 273);   // 切到「录屏」页
            click(793, 533);   // 倒计时选「5 秒」
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
            "录屏与音频",       "HALOCAP 离线运行", "设置与快捷键"
        };
        std::vector<core::ocr::OcrLine> lines;
        for (int i = 0; i < 6; ++i) {
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
        const auto renderWindow = [](HWND window, const std::wstring& path) {
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
                // 只数预览区：右侧「复制全部」按钮本来就是 #007AFF，会污染蓝色计数。
                const int previewRight = width * 58 / 100 - 12;
                for (int y = 52; y < height - 12; ++y) {
                    for (int x = 12; x < previewRight; ++x) {
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
        const bool opened = searchWindow->Create(nullptr, image.data(), kImageWidth, kImageHeight,
                                                 result);
        bool counterOk = false;
        bool wrapOk = false;
        bool caseOk = false;
        bool noneOk = false;
        bool clearOk = false;
        bool repaintOk = false;
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
            highlighted = renderWindow(searchWindow->Handle(), options.ocrSearchSelftestPath);
            searchWindow->SetQueryForTest(L"");
            Sleep(200);
            cleared = renderWindow(searchWindow->Handle(),
                                   options.ocrSearchSelftestPath + L".cleared.png");
            repaintOk = highlighted.green > 0 && highlighted.blue > 0 && cleared.green == 0 &&
                        cleared.blue == 0;
            clearOk = searchWindow->MatchesForTest().empty() &&
                      status().find(L"左侧为识别区域") == 0;
            searchWindow.reset();
        }

        AddFlag(report, "APP_OCR_SEARCH_WINDOW", opened);
        AddFlag(report, "APP_OCR_SEARCH_COUNTER", counterOk);
        AddFlag(report, "APP_OCR_SEARCH_WRAP", wrapOk);
        AddFlag(report, "APP_OCR_SEARCH_CASE", caseOk);
        AddFlag(report, "APP_OCR_SEARCH_NONE", noneOk);
        AddFlag(report, "APP_OCR_SEARCH_CLEAR", clearOk);
        AddFlag(report, "APP_OCR_SEARCH_REPAINT", repaintOk);
        AddInt(report, "APP_OCR_SEARCH_GREEN_ON", highlighted.green);
        AddInt(report, "APP_OCR_SEARCH_BLUE_ON", highlighted.blue);
        AddInt(report, "APP_OCR_SEARCH_GREEN_OFF", cleared.green);
        AddInt(report, "APP_OCR_SEARCH_BLUE_OFF", cleared.blue);
        ocrSearchSelftestOk = opened && counterOk && wrapOk && caseOk && noneOk && clearOk &&
                              repaintOk;
        log.Info(ocrSearchSelftestOk ? "ocr search selftest: ok" : "ocr search selftest: failed");
        running = false;
    }

    bool settingsSelftestOk = true;
    if (!options.settingsSelftestPath.empty()) {
        settingsSelftestOk = false;
        openSettings();
        if (settingsWindow != nullptr && settingsWindow->Valid() && options.settingsSelftestPage >= 0) {
            settingsWindow->ShowPageForTest(options.settingsSelftestPage);
        }
        Sleep(350);

        const bool opened = settingsWindow != nullptr && settingsWindow->Valid();
        bool proof = false;
        if (opened) {
            halocap::platform::win32::DesktopImage desktop;
            proof = halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
                    halocap::platform::win32::SaveBgraPng(
                        options.settingsSelftestPath, desktop.bgra.data(), desktop.width,
                        desktop.height, desktop.width * 4);
            settingsWindow->Close();
        }

        AddFlag(report, "APP_SETTINGS_WINDOW", opened);
        AddFlag(report, "APP_SETTINGS_PROOF", proof);
        settingsSelftestOk = opened && proof;
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
            halocap::platform::win32::WriteUnicodeTextToClipboard("#1234");
        if (wrote) {
            pinClipboard();
        }
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

        const bool pinned = pins.size() > afterCurrent;
        halocap::platform::win32::DesktopImage desktop;
        const bool proof =
            halocap::platform::win32::GrabPrimaryDesktop(desktop) &&
            halocap::platform::win32::SaveBgraPng(options.pinSelftestPath, desktop.bgra.data(),
                                                  desktop.width, desktop.height,
                                                  desktop.width * 4);

        AddFlag(report, "APP_PIN_CLIPBOARD", wrote);
        AddFlag(report, "APP_PIN_WINDOW", pinned);
        AddFlag(report, "APP_PIN_PROOF", proof);
        AddInt(report, "APP_PIN_COUNT", static_cast<long long>(pins.size() - before));

        pinSelftestOk = wrote && pinned && proof;
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
        radialSelftestOk = opened && proof && cursorNormal;
        log.Info(radialSelftestOk ? "radial selftest: ok" : "radial selftest: failed");
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
            const core::TrayCommand command = tray.PopupMenu(trayState);
            if (command != core::TrayCommand::None) {
                onAction(command);
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

        if (const core::TrayCommand command = tray.PollCommand();
            command != core::TrayCommand::None) {
            onAction(command);
        }

        // OCR 结果窗口关闭后释放；引擎空闲 60 秒后卸载，回到空闲内存预算内。
        if (ocrWindow != nullptr && !ocrWindow->Valid()) {
            ocrWindow.reset();
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

        // 录制泵：与消息循环同一个线程、同一个 30 ms 节奏。采集侧丢设备时这里会返回
        // false，录制作废并给出提示，而不是让 app 卡住。
        if (recordingHud != nullptr) {
            recordingHud->Update(recording.MediaTimeMs(), recording.Paused());
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
                refreshTray();
                tray.Notify("HaloCap", "录制中断：采集源丢失");
                if (recordRequested) {
                    running = false;
                }
            }
        }

        if (recordRequested && recordingActive &&
            static_cast<std::int64_t>(GetTickCount64()) >= recordDeadlineMs) {
            recordStopped = recording.Stop(recordStats);
            recordingActive = false;
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
    AddFlag(report, "APP_PIN_SELFTEST", pinSelftestOk);
    AddFlag(report, "APP_RADIAL_SELFTEST", radialSelftestOk);
    AddFlag(report, "APP_WINDOW_PICKER_SELFTEST", windowPickerSelftestOk);
    AddFlag(report, "APP_RADIAL_CLICK_SELFTEST", radialClickOk);
    ok = ok && screenshotSelftestOk && ocrSelftestOk && settingsSelftestOk && pinSelftestOk &&
         radialSelftestOk && windowPickerSelftestOk && hotkeySelftestOk && ocrSearchSelftestOk &&
         hotkeyLiveOk;

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
