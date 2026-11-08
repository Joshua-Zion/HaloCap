// WGC 窗口录制的端到端：一个自己画的窗口 → Windows.Graphics.Capture → 常驻纹理 →
// NV12 → IMFSinkWriter → MP4。窗口内容每 33 ms 变一次颜色，所以每一帧都是新内容、
// 不存在「静止窗口没有帧」的干扰。
//
// 参数：秒数 / 输出文件
//   wgc_probe.exe             3 秒、./wgc-probe.mp4
//   wgc_probe.exe 6 D:\tmp\w.mp4
#include "d2d_render_backend.h"
#include "mf_h264_encoder.h"
#include "recording_session.h"
#include "win32_wgc_capture.h"

#include <windows.h>

#include <mfreadwrite.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace core = halocap::core;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::MfH264Encoder;
using halocap::platform::win32::Win32WgcCapture;

namespace {

constexpr wchar_t kWindowClass[] = L"HaloCapWgcTarget";
constexpr int kWindowWidth = 960;
constexpr int kWindowHeight = 540;

COLORREF g_color = RGB(30, 60, 120);

LRESULT CALLBACK TargetProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        HBRUSH brush = CreateSolidBrush(g_color);
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateTargetWindow() {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &TargetProc;
    description.hInstance = GetModuleHandleW(nullptr);
    description.hbrBackground = nullptr;
    description.lpszClassName = kWindowClass;
    RegisterClassExW(&description);

    return CreateWindowExW(0, kWindowClass, L"HaloCap WGC target", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, kWindowWidth, kWindowHeight, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

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

std::int64_t FileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return -1;
    }
    return (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) |
           static_cast<std::int64_t>(data.nFileSizeLow);
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

std::string AbsolutePath(const std::string& utf8Path) {
    const std::wstring wide = Utf8ToWide(utf8Path);
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(wide.c_str(), MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return utf8Path;
    }
    return WideToUtf8(buffer);
}

bool ProbeFile(const std::wstring& path, bool& hasVideo, std::int64_t& durationMs) {
    hasVideo = false;
    durationMs = 0;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IMFMediaType> videoType;
    hasVideo = SUCCEEDED(reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &videoType));
    PROPVARIANT duration{};
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        durationMs = static_cast<std::int64_t>(duration.uhVal.QuadPart / 10000);
    }
    PropVariantClear(&duration);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
    // 第 3 个参数是窗口类名：给了就采那个外部窗口（用来区分「本进程窗口」与「环境限制」）。
    const std::string targetClass = argc > 3 ? argv[3] : std::string();
    const std::string outputPath = argc > 2 ? argv[2] : "wgc-probe.mp4";

    HWND target = targetClass.empty()
                      ? CreateTargetWindow()
                      : FindWindowW(Utf8ToWide(targetClass).c_str(), nullptr);
    if (!targetClass.empty()) {
        std::printf("EXTERNAL_TARGET_CLASS=%s hwnd=%p\n", targetClass.c_str(),
                    static_cast<void*>(target));
    }
    if (target == nullptr) {
        std::printf("TARGET_WINDOW=FAILED lastError=%lu\n",
                    static_cast<unsigned long>(GetLastError()));
        return 2;
    }
    ShowWindow(target, SW_SHOWNOACTIVATE);
    InvalidateRect(target, nullptr, TRUE);
    PumpMessages();
    std::printf("TARGET_WINDOW=1\n");

    if (!MfH264Encoder::StartupFoundation()) {
        std::printf("MFSTARTUP=FAILED\n");
        return 2;
    }

    D2dRenderBackend backend;
    if (!backend.Initialize() || backend.Device() == nullptr) {
        std::printf("BACKEND=FAILED hr=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        MfH264Encoder::ShutdownFoundation();
        return 2;
    }

    Win32WgcCapture capture(backend);
    MfH264Encoder encoder(backend);
    if (!encoder.Initialize()) {
        std::printf("ENCODER_INIT=FAILED\n");
        MfH264Encoder::ShutdownFoundation();
        return 2;
    }

    if (!capture.OpenWindow(target)) {
        std::printf("WGC_OPEN=FAILED kind=%s code=0x%08lX\n", capture.LastErrorKind(),
                    static_cast<unsigned long>(capture.LastErrorKind() ? 0 : 0));
        std::printf("WGC_PROBE=FAILED\n");
        MfH264Encoder::ShutdownFoundation();
        return 1;
    }
    std::printf("WGC_OPEN=OK bounds=%d,%d..%d,%d\n", capture.DesktopBounds().left,
                capture.DesktopBounds().top, capture.DesktopBounds().right,
                capture.DesktopBounds().bottom);

    core::RecordingOptions options;
    options.captureWindow = target;   // 只用于日志语义；capture 已经自己打开了
    options.targetFps = 30;
    options.outputPathUtf8 = AbsolutePath(outputPath);

    core::RecordingSession session(capture, encoder, nullptr, nullptr);
    if (!session.Start(options, QpcNowUs())) {
        std::printf("SESSION_START=FAILED kind=%s\n", session.LastErrorKind());
        std::printf("WGC_PROBE=FAILED\n");
        MfH264Encoder::ShutdownFoundation();
        return 1;
    }

    const std::int64_t startUs = QpcNowUs();
    const std::int64_t durationUs = static_cast<std::int64_t>(seconds) * 1000000;
    std::int64_t lastPaintUs = startUs;
    int colorStep = 0;
    bool alive = true;

    while (QpcNowUs() - startUs < durationUs) {
        PumpMessages();
        const std::int64_t now = QpcNowUs();

        if (now - lastPaintUs >= 33'000) {
            lastPaintUs = now;
            ++colorStep;
            g_color = RGB((colorStep * 37) % 256, (colorStep * 71) % 256, (colorStep * 113) % 256);
            InvalidateRect(target, nullptr, TRUE);
        }

        if (!session.Pump(now)) {
            std::printf("SESSION_ENDED kind=%s\n", session.LastErrorKind());
            alive = false;
            break;
        }
    }

    core::RecordingStats stats;
    const bool stopped = session.Stop(QpcNowUs(), stats);
    const std::int64_t bytes = FileSizeBytes(Utf8ToWide(options.outputPathUtf8));

    bool hasVideo = false;
    std::int64_t fileDurationMs = 0;
    const bool probed = ProbeFile(Utf8ToWide(options.outputPathUtf8), hasVideo, fileDurationMs);

    std::printf("WGC_FRAMES_SUBMITTED=%lld\n", static_cast<long long>(stats.framesSubmitted));
    std::printf("WGC_WAKEUPS=%lld\n", static_cast<long long>(stats.wakeups));
    std::printf("WGC_CONTENT_FRAMES=%lld\n", static_cast<long long>(stats.contentFrames));
    std::printf("WGC_GATE_DROPPED=%lld\n", static_cast<long long>(stats.framesGateDropped));
    std::printf("WGC_REJECTED=%lld\n", static_cast<long long>(stats.framesRejected));
    std::printf("WGC_RATIO=%.4f\n", stats.SubmitRatio(options.targetFps));
    std::printf("WGC_BYTES=%lld\n", static_cast<long long>(bytes));
    std::printf("WGC_FILE_PROBED=%d VIDEO_TRACK=%d DURATION_MS=%lld\n", probed ? 1 : 0,
                hasVideo ? 1 : 0, static_cast<long long>(fileDurationMs));

    DestroyWindow(target);

    const bool ok = alive && stopped && stats.framesSubmitted > 0 && hasVideo && bytes > 0;
    std::printf("WGC_PROBE=%s\n", ok ? "OK" : "FAILED");
    MfH264Encoder::ShutdownFoundation();
    return ok ? 0 : 1;
}
