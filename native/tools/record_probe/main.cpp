// S2 的第一条端到端：桌面复制 → 常驻纹理 → 录制会话 → NV12 → IMFSinkWriter → MP4。
//
// 它跑的是产品要用的那三块真 seam（Win32DesktopDuplication / MfH264Encoder / RecordingSession），
// 自己不重写任何采集或编码逻辑——否则量出来的数字只说明探针能跑。
//
// 会搬一个 240×160 的黑方块制造画面变化（与 capture_probe 同一条办法）；不搬的话静态桌面
// 一个内容帧都不会有，「不丢帧」也就无从谈起（1.x 的教训：20 秒静态桌面只送 149 帧）。
//
// 参数：秒数 / 输出序号 / 输出文件
//   record_probe.exe                      5 秒、输出 0、./record-probe.mp4
//   record_probe.exe 10 0 D:\tmp\a.mp4
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"
#include "mf_h264_encoder.h"
#include "recording_session.h"
#include "wasapi_audio_source.h"

#include <windows.h>

#include <mfapi.h>
#include <mfreadwrite.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace core = halocap::core;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::MfH264Encoder;
using halocap::platform::win32::Win32DesktopDuplication;

namespace {

constexpr wchar_t kMoverClass[] = L"HaloCapRecordMover";
constexpr int kMoverWidth = 240;
constexpr int kMoverHeight = 160;

LRESULT CALLBACK MoverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        HBRUSH brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(dc, &paint.rcPaint, brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateMover() {
    static const ATOM atom = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &MoverProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        description.lpszClassName = kMoverClass;
        return RegisterClassExW(&description);
    }();

    if (atom == 0) {
        return nullptr;
    }

    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kMoverClass, L"HaloCap record mover",
                           WS_POPUP | WS_VISIBLE, 40, 40, kMoverWidth, kMoverHeight, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
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

double PrivateMemoryMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.PrivateUsage) / 1048576.0;
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

// 编码器说「写完了」不等于文件里有那条流。用源读取器把音轨与时长读回来，
// 这是「MP4 里真的有 AAC」唯一不靠猜的判据。
bool ProbeFile(const std::wstring& path, bool& hasVideo, bool& hasAudio, std::int64_t& durationMs,
               int& width, int& height) {
    hasVideo = false;
    hasAudio = false;
    durationMs = 0;

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> videoType;
    hasVideo = SUCCEEDED(reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &videoType));
    if (hasVideo) {
        UINT32 videoWidth = 0;
        UINT32 videoHeight = 0;
        if (SUCCEEDED(MFGetAttributeSize(videoType.Get(), MF_MT_FRAME_SIZE, &videoWidth,
                                         &videoHeight))) {
            width = static_cast<int>(videoWidth);
            height = static_cast<int>(videoHeight);
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaType> audioType;
    hasAudio = SUCCEEDED(reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audioType));

    PROPVARIANT duration{};
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        durationMs = static_cast<std::int64_t>(duration.uhVal.QuadPart / 10000);
    }
    PropVariantClear(&duration);
    return true;
}

// 合成采集源：本机只有 1080p 输出，4K30 的峰值内存不能靠换显示器来量。
// 它造一张指定尺寸的 BGRA 纹理（RT|SRV，与 WGC 常驻纹理同一形状），每帧清成轮换的颜色，
// 然后用同一份 RecordingSession 与编码器跑——量到的就是产品那条录制管线的内存。
class SyntheticCapture final : public core::ICaptureSource {
public:
    SyntheticCapture(D2dRenderBackend& backend, int widthPx, int heightPx)
        : backend_(backend), widthPx_(widthPx), heightPx_(heightPx) {}

    ~SyntheticCapture() override { Close(); }

    bool Open(int) override {
        // 一张预先填好颜色的纹理，与桌面复制的「一张常驻纹理」同数量级；之后每帧只换时间戳，
        // 不再碰 GPU：采集源要是每帧都 ClearRenderTargetView + Flush，就会和写入器在同一台
        // 设备上互相等（实测泵会从微秒级掉到几十毫秒级）。真实采集也不在 CPU 侧造画面。
        const float colors[3][4] = {
            { 0.15f, 0.25f, 0.55f, 1.0f },
            { 0.55f, 0.15f, 0.25f, 1.0f },
            { 0.20f, 0.55f, 0.25f, 1.0f },
        };

        for (int i = 0; i < 1; ++i) {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(widthPx_);
            description.Height = static_cast<UINT>(heightPx_);
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(backend_.Device()->CreateTexture2D(&description, nullptr, &texture))) {
                errorKind_ = "hr";
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(backend_.Device()->CreateRenderTargetView(texture.Get(), nullptr, &rtv))) {
                errorKind_ = "hr";
                return false;
            }
            backend_.D3dContext()->ClearRenderTargetView(rtv.Get(), colors[i]);

            Microsoft::WRL::ComPtr<IDXGISurface> surface;
            if (FAILED(texture.As(&surface))) {
                errorKind_ = "hr";
                return false;
            }

            D2D1_BITMAP_PROPERTIES1 properties{};
            properties.pixelFormat =
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
            properties.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            if (FAILED(backend_.Dc()->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                                  &bitmap))) {
                errorKind_ = "hr";
                return false;
            }

            handles_[i] = backend_.RegisterGpuImage(bitmap.Get());
            if (handles_[i] == 0) {
                errorKind_ = "hr";
                return false;
            }
        }

        backend_.D3dContext()->Flush();
        errorKind_ = "ok";
        return true;
    }

    void Close() override {
        for (core::ImageHandle& handle : handles_) {
            if (handle != 0) {
                backend_.ReleaseImage(handle);
                handle = 0;
            }
        }
    }

    bool WaitForUpdate(std::int64_t timeoutMs, core::FrameUpdate& out) override {
        if (handles_[0] == 0) {
            errorKind_ = "invalid";
            return false;
        }

        // 按 30 fps 的节拍交帧：真实采集也是「到点才有新帧」，而泵循环跑得比它快得多。
        const std::int64_t nowUs = QpcNowUs();
        if (lastFrameUs_ != 0) {
            const std::int64_t nextUs = lastFrameUs_ + 33'333;
            if (nowUs < nextUs) {
                const std::int64_t waitUs =
                    std::min<std::int64_t>(nextUs - nowUs, timeoutMs * 1000);
                if (waitUs > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(waitUs));
                }
                if (nextUs - nowUs > timeoutMs * 1000) {
                    errorKind_ = "timeout";
                    return false;
                }
            }
        }

        lastFrameUs_ = QpcNowUs();

        out = core::FrameUpdate{};
        out.timestampUs = lastFrameUs_;
        out.accumulatedFrames = 1;
        out.contentChanged = true;
        out.target = handles_[0];
        ++frameIndex_;
        out.widthPx = widthPx_;
        out.heightPx = heightPx_;
        out.desktopBounds = core::IntRect{ 0, 0, widthPx_, heightPx_ };
        out.dirtyRects.assign(1, core::IntRect{ 0, 0, widthPx_, heightPx_ });
        errorKind_ = "ok";
        return true;
    }

    const char* LastErrorKind() const override { return errorKind_; }

    core::IntRect DesktopBounds() const override {
        return core::IntRect{ 0, 0, widthPx_, heightPx_ };
    }

private:
    D2dRenderBackend& backend_;
    int widthPx_ = 0;
    int heightPx_ = 0;
    int frameIndex_ = 0;
    std::int64_t lastFrameUs_ = 0;
    core::ImageHandle handles_[3] = { 0, 0, 0 };
    const char* errorKind_ = "none";
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // 卡住时也要看得到走到哪一步：块缓冲会把现场一起吞掉。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        std::printf("DPI_AWARE=FAILED lastError=%lu\n", static_cast<unsigned long>(GetLastError()));
    }

    if (argc > 2 && std::string_view(argv[1]) == "--inspect") {
        if (!MfH264Encoder::StartupFoundation()) {
            std::printf("MFSTARTUP=FAILED\n");
            return 2;
        }
        bool hasVideo = false;
        bool hasAudio = false;
        std::int64_t duration = 0;
        int width = 0;
        int height = 0;
        const bool ok = ProbeFile(Utf8ToWide(argv[2]), hasVideo, hasAudio, duration, width, height);
        std::printf("FILE_PROBED=%d VIDEO_TRACK=%d AUDIO_TRACK=%d WIDTH=%d HEIGHT=%d DURATION_MS=%lld\n",
                    ok ? 1 : 0, hasVideo ? 1 : 0, hasAudio ? 1 : 0, width, height,
                    static_cast<long long>(duration));
        MfH264Encoder::ShutdownFoundation();
        return ok ? 0 : 1;
    }

    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    const int outputIndex = argc > 2 ? std::atoi(argv[2]) : 0;
    const std::string outputPath = argc > 3 ? argv[3] : "record-probe.mp4";
    // 第 4 个参数写 static 就不搬方块：用来验证静态桌面的保活帧（否则文件会因 0 样本而坏）。
    const bool staticDesktop = argc > 4 && std::string_view(argv[4]) == "static";
    // 第 5 个参数写 noaudio 就只录视频（对照，也方便没有音频设备的机器）。
    const bool noAudio = argc > 5 && std::string_view(argv[5]) == "noaudio";

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

    // 合成源模式：synthetic:3840x2160 —— 本机只有 1080p 输出，4K30 的内存只能这样量。
    bool syntheticMode = false;
    int syntheticWidth = 0;
    int syntheticHeight = 0;
    std::unique_ptr<SyntheticCapture> synthetic;
    {
        const std::string_view mode = argc > 4 ? argv[4] : "";
        const std::string_view prefix = "synthetic:";
        if (mode.substr(0, prefix.size()) == prefix) {
            int width = 0;
            int height = 0;
            if (std::sscanf(std::string(mode.substr(prefix.size())).c_str(), "%dx%d", &width,
                            &height) == 2 &&
                width >= 2 && height >= 2) {
                syntheticMode = true;
                syntheticWidth = width & ~1;
                syntheticHeight = height & ~1;
            }
        }
    }

    Win32DesktopDuplication duplication(backend);
    if (!syntheticMode) {
        if (!duplication.Initialize()) {
            std::printf("CAPTURE_INIT=FAILED\n");
            MfH264Encoder::ShutdownFoundation();
            return 2;
        }
        std::printf("OUTPUTS_SYSTEM=%d OUTPUTS_CAPTURABLE=%d\n", duplication.TotalOutputCount(),
                    duplication.OutputCount());
    } else {
        synthetic = std::make_unique<SyntheticCapture>(backend, syntheticWidth, syntheticHeight);
        if (!synthetic->Open(0)) {
            std::printf("SYNTHETIC_OPEN=FAILED kind=%s\n", synthetic->LastErrorKind());
            MfH264Encoder::ShutdownFoundation();
            return 2;
        }
        std::printf("SYNTHETIC_SIZE=%dx%d\n", syntheticWidth, syntheticHeight);
    }

    core::ICaptureSource& capture =
        syntheticMode ? static_cast<core::ICaptureSource&>(*synthetic)
                      : static_cast<core::ICaptureSource&>(duplication);

    MfH264Encoder encoder(backend);
    if (!encoder.Initialize()) {
        std::printf("ENCODER_INIT=FAILED hr=0x%08lX\n",
                    static_cast<unsigned long>(encoder.LastErrorCode()));
        MfH264Encoder::ShutdownFoundation();
        return 2;
    }

    core::RecordingOptions options;
    options.outputIndex = outputIndex;
    options.targetFps = 30;
    options.outputPathUtf8 = AbsolutePath(outputPath);
    options.audioEnabled = !noAudio;

    // 两路都开：系统声音（回环）+ 麦克风。任何一路打不开都不影响录制，只少一路声音。
    halocap::platform::win32::WasapiAudioSource systemAudio(
        core::AudioSourceKind::SystemLoopback);
    halocap::platform::win32::WasapiAudioSource microphone(core::AudioSourceKind::Microphone);

    core::RecordingSession session(capture, encoder, &systemAudio, &microphone);

    bool started = false;
    int attempts = 0;
    for (; attempts < 4 && !started; ++attempts) {
        started = session.Start(options, QpcNowUs());
        if (!started) {
            const char* kind = session.LastErrorKind();
            std::printf("OPEN_ATTEMPT=%d FAILED kind=%s\n", attempts + 1, kind == nullptr ? "?" : kind);
            // 只有复制权被占用值得重试；尺寸/路径/编码器的问题重试也不会变。
            if (kind == nullptr || std::string_view(kind) != "busy") {
                break;
            }
            const std::int64_t backoffUs = core::BackoffUs(attempts, 250'000, 4'000'000);
            Sleep(static_cast<DWORD>(backoffUs / 1000));
        }
    }
    std::printf("OPEN_ATTEMPTS=%d\n", attempts);

    if (!started) {
        std::printf("RECORD_PROBE=BLOCKED kind=%s\n", session.LastErrorKind());
        MfH264Encoder::ShutdownFoundation();
        return 0;
    }

    std::printf("RECORD_STARTED=1 TARGET_FPS=%d OUTPUT=%s\n", options.targetFps,
                options.outputPathUtf8.c_str());
    const std::int64_t startUs = QpcNowUs();
    const std::int64_t durationUs = static_cast<std::int64_t>(seconds) * 1000000;
    std::int64_t lastMoveUs = startUs;
    int moveStep = 0;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HWND mover = staticDesktop ? nullptr : CreateMover();
    std::printf("RECORD_STATIC=%d\n", staticDesktop ? 1 : 0);

    bool alive = true;
    double peakPrivateMb = 0.0;
    while (QpcNowUs() - startUs < durationUs) {
        PumpMessages();
        const std::int64_t now = QpcNowUs();
        const bool pumpOk = session.Pump(now);
        if (!pumpOk) {
            std::printf("SESSION_ENDED kind=%s\n", session.LastErrorKind());
            alive = false;
            break;
        }

        // 峰值比结束时的快照有用：驱动与写入器的缓冲会在中途顶起来再落回去。
        const double privateMb = PrivateMemoryMb();
        if (privateMb > peakPrivateMb) {
            peakPrivateMb = privateMb;
        }

        // 每 33 ms 挪一步：30 fps 下每一帧都该有变化。
        if (mover != nullptr && now - lastMoveUs >= 33'000) {
            lastMoveUs = now;
            const int spanX = screenWidth > kMoverWidth ? screenWidth - kMoverWidth : 1;
            const int spanY = screenHeight > kMoverHeight ? screenHeight - kMoverHeight : 1;
            const int x = (moveStep * 37) % spanX;
            const int y = (moveStep * 53) % spanY;
            SetWindowPos(mover, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ++moveStep;
        }
    }

    if (mover != nullptr) {
        DestroyWindow(mover);
    }

    core::RecordingStats stats;
    const bool stopped = session.Stop(QpcNowUs(), stats);
    const std::int64_t bytes = FileSizeBytes(Utf8ToWide(options.outputPathUtf8));

    std::printf("RECORD_WALL_MS=%lld\n", static_cast<long long>(stats.wallTimeUs / 1000));
    std::printf("RECORD_MEDIA_MS=%lld\n", static_cast<long long>(stats.mediaTimeUs / 1000));
    std::printf("RECORD_WAKEUPS=%lld\n", static_cast<long long>(stats.wakeups));
    std::printf("RECORD_CONTENT_FRAMES=%lld\n", static_cast<long long>(stats.contentFrames));
    std::printf("RECORD_SUBMITTED=%lld\n", static_cast<long long>(stats.framesSubmitted));
    std::printf("RECORD_GATE_DROPPED=%lld\n", static_cast<long long>(stats.framesGateDropped));
    std::printf("RECORD_REJECTED=%lld\n", static_cast<long long>(stats.framesRejected));
    std::printf("RECORD_DIRTY_PIXELS=%lld\n", static_cast<long long>(stats.dirtyPixelSum));
    std::printf("RECORD_RATIO=%.4f\n", stats.SubmitRatio(options.targetFps));
    std::printf("RECORD_HARDWARE_REQUESTED=%d\n", stats.hardwareRequested ? 1 : 0);
    std::printf("RECORD_HARDWARE_ENCODED=%d\n", stats.hardwareEncoded ? 1 : 0);
    std::printf("AUDIO_ACTIVE=%d\n", stats.audioActive ? 1 : 0);
    std::printf("AUDIO_PACKETS=%lld\n", static_cast<long long>(stats.audioPackets));
    std::printf("AUDIO_FRAMES=%lld\n", static_cast<long long>(stats.audioFrames));
    std::printf("AUDIO_BLOCKS=%lld\n", static_cast<long long>(stats.audioBlocks));
    std::printf("AUDIO_SILENCE_FRAMES=%lld\n", static_cast<long long>(stats.audioSilenceFrames));
    std::printf("AUDIO_GAP_FRAMES=%lld\n", static_cast<long long>(stats.audioGapFrames));
    std::printf("AUDIO_LATE_FRAMES=%lld\n", static_cast<long long>(stats.audioLateFrames));

    bool hasVideo = false;
    bool hasAudioTrack = false;
    std::int64_t fileDurationMs = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    const bool probed = ProbeFile(Utf8ToWide(options.outputPathUtf8), hasVideo, hasAudioTrack,
                                  fileDurationMs, videoWidth, videoHeight);
    std::printf("RECORD_BYTES=%lld\n", static_cast<long long>(bytes));
    std::printf("FILE_PROBED=%d FILE_VIDEO_TRACK=%d FILE_AUDIO_TRACK=%d FILE_DURATION_MS=%lld\n",
                probed ? 1 : 0, hasVideo ? 1 : 0, hasAudioTrack ? 1 : 0,
                static_cast<long long>(fileDurationMs));
    std::printf("RECORD_PRIVATE_MB=%.1f\n", PrivateMemoryMb());
    std::printf("RECORD_PEAK_PRIVATE_MB=%.1f\n", peakPrivateMb);
    std::printf("RECORD_STOP_OK=%d\n", stopped ? 1 : 0);

    // S2 的 4K30 目标是峰值 ≤ 350 MB。
    constexpr double k4kPeakBudgetMb = 350.0;
    const bool memoryOk = peakPrivateMb > 0.0 && peakPrivateMb <= k4kPeakBudgetMb;
    std::printf("RECORD_4K_MEMORY_OK=%d\n", syntheticMode ? (memoryOk ? 1 : 0) : -1);

    const bool ok = alive && stopped && stats.framesSubmitted > 0 && bytes > 0 &&
                    hasVideo && (!options.audioEnabled || hasAudioTrack) &&
                    (!syntheticMode || memoryOk);
    std::printf("RECORD_PROBE=%s\n", ok ? "OK" : "FAILED");
    MfH264Encoder::ShutdownFoundation();
    return ok ? 0 : 1;
}
