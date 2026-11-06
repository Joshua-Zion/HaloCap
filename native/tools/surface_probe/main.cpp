// S1 的上屏路径验证：真窗口 + DirectComposition 合成链 + 预乘 alpha 的合成交换链，量 Present 成本与私有内存。
// 默认把窗口摆在屏幕外（坐标 -32000），且「不抢焦点 + 不吃鼠标」，跑起来桌面上看不到任何东西。
// 第 4 个参数：0=屏外可见，1=屏上可见，2=屏上但不显示。
//
// 一处必须说清的限制：WS_EX_NOREDIRECTIONBITMAP 的窗口没有重定向位图，PrintWindow 这类 CPU 侧手段
// 拿不到它的内容。所以「屏幕上真的看得见」不在本工具的验证范围内——显示列表内容本身已由 frame_cost
// 逐字节验过，这里只证到 Present 成功；上屏确认需要一次真实截图。
#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "win32_surface_window.h"

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

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

core::FramePlan BuildProbePlan(const core::TextCell& text) {
    core::FramePlan plan;
    plan.Add(text.ToBlit());
    plan.Add(core::FillRect{ core::Rect{ 200.0f, 200.0f, 900.0f, 700.0f },
                             core::Color{ 0.1f, 0.6f, 1.0f, 0.85f } });
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

    const core::FramePlan plan = BuildProbePlan(text);
    Clock clock;

    clock.Start();
    for (int frame = 0; frame < frames; ++frame) {
        backend.Render(plan, core::IntRect{});
        backend.Present(false);
        PumpMessages();
    }
    const double asyncMs = clock.ElapsedMs() / frames;

    const int syncFrames = frames < 120 ? frames : 120;
    clock.Start();
    for (int frame = 0; frame < syncFrames; ++frame) {
        backend.Render(plan, core::IntRect{});
        backend.Present(true);
        PumpMessages();
    }
    const double syncMs = clock.ElapsedMs() / syncFrames;

    std::printf("PRESENT_ASYNC_MS_PER_FRAME=%.3f\n", asyncMs);
    std::printf("PRESENT_SYNC_MS_PER_FRAME=%.3f\n", syncMs);
    std::printf("PRESENT_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
    std::printf("WORKING_SET_MB=%.0f PRIVATE_MEMORY_MB=%.0f\n", MemoryMb(false), MemoryMb(true));
    std::printf("SURFACE_BYTES_MB=%.0f (2 back buffers + offscreen target + staging)\n",
                static_cast<double>(width * height * 4 * 4) / 1048576.0);

    // 拆掉再量一次，看合成链的资源有没有真的还回去。
    const double withChain = MemoryMb(true);
    backend.DetachWindow();
    window.SetVisible(false);
    PumpMessages();
    std::printf("PRIVATE_MEMORY_DETACH_MB=%.0f (attach 后是 %.0f)\n", MemoryMb(true), withChain);

    const bool clean = backend.LastHr() == S_OK;
    std::printf("UNVERIFIED=屏幕上是否真的看得见（要一次真实上屏截图）\n");
    std::printf("OUTCOME=%s\n", clean ? "PRESENTED" : "FAILED");
    return clean ? 0 : 2;
}
