// S1 的上屏路径验证：真窗口 + DirectComposition 合成链 + 预乘 alpha 的合成交换链。
// 量 Present 成本与私有内存，并用「隐藏 / 显示 / 再隐藏」三段桌面抓屏证明像素真的上了屏。
//
// 第 4 个参数：0=屏外可见（默认，桌面上什么都看不到），1=屏上可见并做 A/B 抓屏，2=屏上但不显示。
// mode 1 会把窗口真实显示在 (0,0) 约 0.7 秒。这一步必须上屏：WS_EX_NOREDIRECTIONBITMAP 的窗口
// 没有重定向位图，PrintWindow 这类「针对窗口」的 CPU 手段拿不到内容，只有桌面合成结果（屏幕 DC 的
// BitBlt）能证明「看得见」。窗口不吃鼠标也不抢焦点。
#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "win32_surface_window.h"

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace core = halocap::core;
using halocap::platform::win32::D2dFontRasterizer;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::Win32SurfaceWindow;

namespace {

constexpr std::string_view kProbeText = "ISurfaceWindow \xE4\xB8\x8A\xE5\xB1\x8F\xE9\xAA\x8C\xE8\xAF\x81";

class Clock {
public:
    void Start() { QueryPerformanceCounter(&begin_); }

    double ElapsedMs() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        static const LARGE_INTEGER frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value;
        }();
        return static_cast<double>(now.QuadPart - begin_.QuadPart) * 1000.0 /
               static_cast<double>(frequency.QuadPart);
    }

private:
    LARGE_INTEGER begin_{};
};

double MemoryMb(bool privateBytes) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))) {
        return -1.0;
    }

    // 与 1.x 环境摘要同一口径：私有内存就是 PrivateUsage（EX2 才有 PrivateWorkingSetSize）。
    const SIZE_T value = privateBytes ? counters.PrivateUsage : counters.WorkingSetSize;
    return static_cast<double>(value) / 1048576.0;
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// 桌面合成结果抓一份到 CPU：这是唯一能证明「合成器真的用了我们那一帧」的入口。
struct DesktopShot {
    int width = 0;
    int height = 0;
    int pitch = 0;
    std::vector<unsigned char> pixels;
};

bool CaptureDesktop(int x, int y, int width, int height, DesktopShot& shot) {
    shot.width = width;
    shot.height = height;
    shot.pitch = width * 4;
    shot.pixels.assign(static_cast<std::size_t>(shot.pitch) * height, 0);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    if (screen == nullptr || memory == nullptr) {
        if (memory != nullptr) {
            DeleteDC(memory);
        }
        if (screen != nullptr) {
            ReleaseDC(nullptr, screen);
        }
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ previous = SelectObject(memory, dib);
    const bool blitted = BitBlt(memory, 0, 0, width, height, screen, x, y,
                                SRCCOPY | CAPTUREBLT) != 0;
    if (blitted) {
        memcpy(shot.pixels.data(), bits, shot.pixels.size());
    }

    SelectObject(memory, previous);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return blitted;
}

void ReadPixel(const DesktopShot& shot, int x, int y, unsigned char out[4]) {
    const std::size_t offset = static_cast<std::size_t>(y) * shot.pitch +
                               static_cast<std::size_t>(x) * 4;
    for (int channel = 0; channel < 4; ++channel) {
        out[channel] = shot.pixels[offset + channel];
    }
}

bool NearColor(const unsigned char pixel[4], unsigned char blue, unsigned char green,
               unsigned char red, int tolerance) {
    return pixel[0] >= blue - tolerance && pixel[0] <= blue + tolerance &&
           pixel[1] >= green - tolerance && pixel[1] <= green + tolerance &&
           pixel[2] >= red - tolerance && pixel[2] <= red + tolerance;
}

int CountNearWhiteInk(const DesktopShot& shot, int x0, int y0, int x1, int y1) {
    int count = 0;
    for (int y = y0; y < y1 && y < shot.height; ++y) {
        for (int x = x0; x < x1 && x < shot.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * shot.pitch +
                                       static_cast<std::size_t>(x) * 4;
            const unsigned char* pixel = &shot.pixels[offset];
            if (pixel[0] > 235 && pixel[1] > 235 && pixel[2] > 235) {
                ++count;
            }
        }
    }
    return count;
}

// 跑一批帧并给出每帧毫秒。paced=true 时每帧先等一个帧槽，这才是 §1.3「不丢帧」要看的数。
double RunFrames(D2dRenderBackend& backend, const core::FramePlan& plan, int frames, bool paced) {
    Clock clock;
    clock.Start();
    for (int frame = 0; frame < frames; ++frame) {
        if (paced) {
            backend.WaitForFrameSlot();
        }
        backend.Render(plan, core::IntRect{});
        backend.Present();
        PumpMessages();
    }
    return clock.ElapsedMs() / frames;
}

// 方块内、但不与斜线/文本重叠的一点：斜线从 (200,200) 走到 (900,700)，取样必须避开它，
// 否则抓到的是白线而不是方块本色。
constexpr int kProbeSampleX = 300;
constexpr int kProbeSampleY = 620;

bool VerifyOnScreen(D2dRenderBackend& backend, Win32SurfaceWindow& window,
                    const core::FramePlan& plan, int width, int height) {
    DesktopShot before{};
    DesktopShot during{};
    DesktopShot after{};

    window.SetVisible(false);
    PumpMessages();
    Sleep(250);
    if (!CaptureDesktop(0, 0, width, height, before)) {
        std::printf("SCREEN_CAPTURE=FAILED lastError=%lu\n",
                    static_cast<unsigned long>(GetLastError()));
        return false;
    }

    window.SetVisible(true);
    // 显示时重新压一次 topmost：ShowWindow 不改 Z 序，别的 topmost 窗口可能压在我们上面。
    SetWindowPos(static_cast<HWND>(window.Handle()), HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // 命中测试临时打开，这样 WindowFromPoint 能告诉我们取样点上到底是谁在最上面。
    window.SetClickThrough(false);
    POINT samplePoint{ kProbeSampleX, kProbeSampleY };
    HWND top = WindowFromPoint(samplePoint);
    const bool oursOnTop = top == static_cast<HWND>(window.Handle());
    RECT ourRect{};
    GetWindowRect(static_cast<HWND>(window.Handle()), &ourRect);
    std::printf("SCREEN_TOP_IS_OURS=%d top=%p ours=%p our_rect=%ld,%ld,%ld,%ld\n",
                oursOnTop ? 1 : 0, top, static_cast<HWND>(window.Handle()), ourRect.left,
                ourRect.top, ourRect.right, ourRect.bottom);

    for (int frame = 0; frame < 8; ++frame) {
        backend.Render(plan, core::IntRect{});
        backend.Present();
        PumpMessages();
    }
    Sleep(250);
    if (!CaptureDesktop(0, 0, width, height, during)) {
        window.SetClickThrough(true);
        return false;
    }
    const int whiteInk = CountNearWhiteInk(during, 220, 240, 660, 280);
    window.SetClickThrough(true);

    window.SetVisible(false);
    PumpMessages();
    Sleep(250);
    if (!CaptureDesktop(0, 0, width, height, after)) {
        return false;
    }

    unsigned char pixelBefore[4]{};
    unsigned char pixelDuring[4]{};
    unsigned char pixelAfter[4]{};
    ReadPixel(before, kProbeSampleX, kProbeSampleY, pixelBefore);
    ReadPixel(during, kProbeSampleX, kProbeSampleY, pixelDuring);
    ReadPixel(after, kProbeSampleX, kProbeSampleY, pixelAfter);

    std::printf("SCREEN_CENTER_BGRA before=%u,%u,%u,%u during=%u,%u,%u,%u after=%u,%u,%u,%u\n",
                pixelBefore[0], pixelBefore[1], pixelBefore[2], pixelBefore[3],
                pixelDuring[0], pixelDuring[1], pixelDuring[2], pixelDuring[3],
                pixelAfter[0], pixelAfter[1], pixelAfter[2], pixelAfter[3]);

    const bool notTealBefore = !NearColor(pixelBefore, 255, 153, 26, 12);
    const bool tealWhileShown = NearColor(pixelDuring, 255, 153, 26, 12);
    const bool notTealAfter = !NearColor(pixelAfter, 255, 153, 26, 12);
    const bool ok = notTealBefore && tealWhileShown && notTealAfter && whiteInk > 5;

    std::printf("SCREEN_TEXT_WHITE_PIXELS=%d SCREEN_VERIFY=%s\n", whiteInk, ok ? "OK" : "FAIL");
    if (!ok) {
        std::printf("  判定：%s%s%s%s\n",
                    notTealBefore ? "" : "before 已经是目标色（说明窗口没真的藏起来或抓错了）",
                    tealWhileShown ? "" : " during 不是目标色（内容没上屏或被遮挡）",
                    notTealAfter ? "" : " after 仍是目标色（隐藏没生效）",
                    whiteInk > 5 ? "" : " 文本白点太少（字形没画出来或被裁掉）");
    }
    return ok;
}

core::FramePlan BuildProbePlan(const core::TextCell& text) {
    core::FramePlan plan;
    // 顺序即图层顺序：先铺不透明方块，文本和斜线才有东西可盖在上面。
    plan.Add(core::FillRect{ core::Rect{ 200.0f, 200.0f, 900.0f, 700.0f },
                             core::Color{ 0.1f, 0.6f, 1.0f, 1.0f } });
    plan.Add(text.ToBlit());
    plan.Add(core::Segment{ core::Point{ 200.0f, 200.0f }, core::Point{ 900.0f, 700.0f },
                            core::Color{ 1.0f, 1.0f, 1.0f, 1.0f }, 3.0f });
    return plan;
}

}

int main(int argc, char** argv) {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        std::printf("DPI_AWARE=FAILED lastError=%lu\n", static_cast<unsigned long>(GetLastError()));
    } else {
        std::printf("DPI_AWARE=PER_MONITOR_V2\n");
    }

    const int width = argc > 1 ? std::atoi(argv[1]) : 1920;
    const int height = argc > 2 ? std::atoi(argv[2]) : 1080;
    const int frames = argc > 3 ? std::atoi(argv[3]) : 240;
    const int place = argc > 4 ? std::atoi(argv[4]) : 0;

    D2dRenderBackend backend;
    if (!backend.Initialize()) {
        std::printf("BACKEND_INITIALIZE_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return 2;
    }

    D2dFontRasterizer rasterizer(backend);
    if (!rasterizer.Initialize()) {
        std::printf("RASTERIZER_INITIALIZE_HR=0x%08lX\n",
                    static_cast<unsigned long>(rasterizer.LastHr()));
        return 2;
    }

    const core::FontSpec spec{ "Segoe UI", 400, 28.0f, core::Color{ 1.0f, 1.0f, 1.0f, 1.0f } };
    const core::TextCell text = rasterizer.LayOut(kProbeText, spec, core::Point{ 220.0f, 240.0f });
    if (text.atlas == 0) {
        std::printf("TEXT_LAYOUT_HR=0x%08lX\n", static_cast<unsigned long>(rasterizer.LastHr()));
        return 2;
    }

    Win32SurfaceWindow window;
    const int anchor = place == 0 ? -32000 : 0;
    const core::WindowBounds bounds{ anchor, anchor, width, height };
    const core::WindowOptions options{ place != 2, false, true, true };
    if (!window.Create(bounds, options)) {
        std::printf("WINDOW_CREATE=FAILED lastError=%lu\n", static_cast<unsigned long>(GetLastError()));
        return 2;
    }

    const LONG_PTR exStyle = window.EffectiveExStyle();
    std::printf("MODE=%d WINDOW_EX_STYLE=0x%0lX NOREDIRECTION=%d NOACTIVATE=%d CLICK_THROUGH=%d VISIBLE=%d\n",
                place, static_cast<unsigned long>(exStyle),
                (exStyle & WS_EX_NOREDIRECTIONBITMAP) != 0 ? 1 : 0,
                (exStyle & WS_EX_NOACTIVATE) != 0 ? 1 : 0,
                (exStyle & WS_EX_TRANSPARENT) != 0 ? 1 : 0,
                window.IsVisible() ? 1 : 0);

    if (!backend.AttachWindow(window.Handle(), width, height)) {
        std::printf("ATTACH_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        std::printf("OUTCOME=ATTACH-FAILED\n");
        return 3;
    }
    std::printf("ATTACH_HR=0x%08lX (DComp device+target+visual+composition swapchain)\n",
                static_cast<unsigned long>(backend.LastHr()));
    std::printf("PACING=%s\n",
                backend.FrameLatencyWaitable() == nullptr ? "none" : "waitable-object latency=1");

    const core::FramePlan plan = BuildProbePlan(text);
    const int pacedFrames = frames < 120 ? frames : 120;

    const double unpacedMs = RunFrames(backend, plan, frames, false);
    const double pacedMs = RunFrames(backend, plan, pacedFrames, true);

    std::printf("PRESENT_UNPACED_MS_PER_FRAME=%.3f\n", unpacedMs);
    std::printf("FRAME_SLOT_PACED_MS_PER_FRAME=%.3f\n", pacedMs);
    std::printf("PRESENT_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
    std::printf("WORKING_SET_MB=%.0f PRIVATE_MEMORY_MB=%.0f\n", MemoryMb(false), MemoryMb(true));
    std::printf("SURFACE_BYTES_MB=%.0f (2 back buffers + offscreen target + staging)\n",
                static_cast<double>(width * height * 4 * 4) / 1048576.0);

    bool screenOk = true;
    if (place == 1) {
        // 只有真的可见时，帧槽节奏才是 §1.3 要的那个数；屏外/隐藏时 DWM 不节流。
        std::printf("VISIBLE_PRESENT_UNPACED_MS_PER_FRAME=%.3f\n",
                    RunFrames(backend, plan, frames, false));
        std::printf("VISIBLE_FRAME_SLOT_PACED_MS_PER_FRAME=%.3f\n",
                    RunFrames(backend, plan, pacedFrames, true));

        screenOk = VerifyOnScreen(backend, window, plan, width, height);
    }

    // 拆掉再量一次，看合成链的资源有没有真的还回去。
    const double withChain = MemoryMb(true);
    backend.DetachWindow();
    window.SetVisible(false);
    PumpMessages();
    std::printf("PRIVATE_MEMORY_DETACH_MB=%.0f (attach 后是 %.0f)\n", MemoryMb(true), withChain);

    const bool clean = backend.LastHr() == S_OK;
    if (place != 1) {
        std::printf("UNVERIFIED=屏幕上是否真的看得见（跑 mode 1 才会做 A/B 抓屏）\n");
    }
    std::printf("OUTCOME=%s\n", (clean && screenOk) ? "PRESENTED" : "FAILED");
    return (clean && screenOk) ? 0 : 2;
}
