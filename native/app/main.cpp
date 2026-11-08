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
#include "font_rasterizer.h"
#include "input_host.h"
#include "recording_session.h"
#include "settings.h"
#include "store.h"
#include "tray_menu.h"

#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"
#include "mf_h264_encoder.h"
#include "wasapi_audio_source.h"
#include "win32_capability.h"
#include "win32_input_host.h"
#include "win32_store.h"
#include "win32_surface_window.h"
#include "win32_tray.h"

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

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
        } else if (argument == "--record-seconds" && i + 1 < argc) {
            options.recordSeconds = std::atoi(argv[++i]);
            if (options.recordSeconds < 0) {
                options.recordSeconds = 0;
            }
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
    bool Start(D2dRenderBackend& backend, const core::AppSettings& settings, AppLog& log, std::string& errorKind) {
        errorKind.clear();

        const std::wstring path = RecordingPathFor(settings);
        if (path.empty()) {
            errorKind = "path";
            return false;
        }
        path_ = WideToUtf8(path);

        log.Info("recording: capture initializing");
        capture_ = std::make_unique<Win32DesktopDuplication>(backend);
        if (!capture_->Initialize()) {
            errorKind = "capture";
            Reset();
            return false;
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

    std::unique_ptr<Win32DesktopDuplication> capture_;
    std::unique_ptr<MfH264Encoder> encoder_;
    std::unique_ptr<WasapiAudioSource> systemAudio_;
    std::unique_ptr<WasapiAudioSource> microphone_;
    std::unique_ptr<core::RecordingSession> session_;
    std::string path_;
};

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

    const core::AppSettings& settings = loaded.settings;

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
    // 剪贴板服务与开机自启服务还没接（S3/S4），这两项先按「不可用 / 未开启」呈现。
    trayState.pinAvailable = false;
    trayState.autoStart = false;

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
    if (options.input) {
        inputStarted = input.Start();
        if (inputStarted) {
            const struct {
                const core::HotkeySpec& spec;
                int token;
                const char* name;
            } hotkeys[] = {
                { settings.radialHotkey, kTokenRadial, "radial" },
                { settings.screenshotHotkey, kTokenScreenshot, "screenshot" },
                { settings.ocrHotkey, kTokenOcr, "ocr" },
                { settings.recordHotkey, kTokenRecord, "record" },
                { settings.pinHotkey, kTokenPin, "pin" },
            };
            for (const auto& hotkey : hotkeys) {
                if (input.RegisterHotkey(hotkey.spec, hotkey.token)) {
                    ++hotkeysRegistered;
                    log.Info(std::string("hotkey: ") + hotkey.name + " = " +
                             core::FormatHotkeyText(hotkey.spec));
                } else {
                    log.Error(std::string("hotkey: ") + hotkey.name + " unavailable (" +
                              input.LastErrorKind() + ")");
                }
            }
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
    bool overlayReady = false;
    std::int64_t overlayAtMs = 0;
    double idlePrivate = -1.0;
    double idleWorkingSet = -1.0;
    double overlayPrivate = -1.0;
    double overlayWorkingSet = -1.0;
    int actions = 0;

    // 产品形态要预热覆盖层：1.x 的 v1.2 起就是这么做的（不预热的话第一次截图会闪一下）。
    // 预热的代价是这 60 MB 常驻 idle，所以 S1 的 80 MB 预算实际是在量「预热之后」。
    if (!options.selftest && options.overlay) {
        overlayReady = overlay.Initialize(log, overlayWidth, overlayHeight);
        log.Info(overlayReady ? "overlay: prewarmed" : "overlay: prewarm failed");
    }

    // ---- 录制入口：托盘「录屏」与录制热键共用这一个开关 ----
    RecordingRunner recording;
    bool recordingActive = false;
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

    auto toggleRecording = [&]() {
        if (!recordingActive) {
            if (!overlayReady) {
                // 录制要复用覆盖层那台 D3D 设备（采集/编码必须同一个设备）。
                overlayReady = overlay.Initialize(log, overlayWidth, overlayHeight);
            }
            std::string kind;
            const bool started = overlayReady && recording.Start(overlay.Backend(), settings, log, kind);
            if (started) {
                recordingActive = true;
                refreshTray();
                log.Info("recording: started -> " + recording.OutputPath());
                tray.Notify("HaloCap", "开始录制");
            } else {
                log.Error("recording: start failed (" +
                          (overlayReady ? kind : std::string("overlay")) + ")");
                tray.Notify("HaloCap", "无法开始录制");
            }
            return;
        }

        core::RecordingStats stats;
        const bool stopped = recording.Stop(stats);
        recordingActive = false;
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
        recordStarted = overlayReady && recording.Start(overlay.Backend(), settings, log, kind);
        if (recordStarted) {
            recordingActive = true;
            refreshTray();
            recordDeadlineMs =
                static_cast<std::int64_t>(GetTickCount64()) + options.recordSeconds * 1000;
            log.Info("recording: started (--record-seconds) -> " + recording.OutputPath());
        } else {
            log.Error("recording: start failed (" +
                      (overlayReady ? kind : std::string("overlay")) + ")");
            running = false;
        }
    }

    auto onAction = [&](core::TrayCommand command) {
        ++actions;
        if (command == core::TrayCommand::Exit) {
            log.Info("action: Exit");
            running = false;
            return;
        }
        if (command == core::TrayCommand::Record) {
            toggleRecording();
            return;
        }
        if (command == core::TrayCommand::PauseRecording) {
            togglePause();
            return;
        }
        log.Info(std::string("action: ") + core::TrayCommandName(command) +
                 " (behaviour lands in S3/S4)");
    };

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
            log.Info("action: left click (radial menu lands in S3)");
            break;
        case Win32Tray::Click::None:
            break;
        }

        if (const core::TrayCommand command = tray.PollCommand();
            command != core::TrayCommand::None) {
            onAction(command);
        }

        // 录制泵：与消息循环同一个线程、同一个 30 ms 节奏。采集侧丢设备时这里会返回
        // false，录制作废并给出提示，而不是让 app 卡住。
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
        // 无人值守录制的判据：起来了、停了、有帧、文件非空。
        ok = ok && recordStarted && recordStopped && recordStats.framesSubmitted > 0 && bytes > 0;
    }

    AddText(report, "APP_SELFTEST", options.selftest ? (ok ? "OK" : "FAILED") : "SKIPPED");

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

    const Options options = ParseOptions(argc, argv);
    if (options.help) {
        PrintUsage();
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
